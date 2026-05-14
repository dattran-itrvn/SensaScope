#!/usr/bin/env python3
"""L2CAP CoC streaming test for SensaPulse.

Bleak's macOS backend doesn't expose L2CAP CoC. We:
  1. Use Bleak for GATT connect + Control writes (small ops).
  2. Monkey-patch Bleak's CBPeripheralDelegate to add the
     `peripheral:didOpenL2CAPChannel:error:` selector so PyObjC delivers
     the L2CAP open callback.
  3. Run NSInputStream events on a dedicated NSRunLoop thread (asyncio
     can't pump NSRunLoop in-place), shovel bytes back via a thread-safe
     queue.
"""
from __future__ import annotations

import asyncio
import sys
import threading
import time
from queue import Queue, Empty

from bleak import BleakClient, BleakScanner
from bleak.backends.corebluetooth.PeripheralDelegate import PeripheralDelegate

import objc
from Foundation import (
    NSObject, NSRunLoop, NSDefaultRunLoopMode, NSDate,
    NSStreamEventOpenCompleted, NSStreamEventHasBytesAvailable,
    NSStreamEventErrorOccurred, NSStreamEventEndEncountered,
)

PSM = 0x0080
CHR_CONTROL = "7e7e0003-3c4f-4b8e-8a8a-5e5e5e5e5e5e"
OP_READ = 0x02


# --- L2CAP open helpers (called from asyncio thread) ---------------------

_l2cap_pending: dict = {}


def _did_open_l2cap_channel(self, peripheral, channel, error):
    key = str(peripheral.identifier())
    fut, loop = _l2cap_pending.get(key, (None, None))
    if fut is None:
        return
    if error:
        loop.call_soon_threadsafe(fut.set_exception,
                                  RuntimeError(str(error)))
    else:
        loop.call_soon_threadsafe(fut.set_result, channel)


PeripheralDelegate.peripheral_didOpenL2CAPChannel_error_ = _did_open_l2cap_channel


async def open_l2cap_channel(client: BleakClient, psm: int):
    peripheral = client._backend._peripheral
    key = str(peripheral.identifier())
    loop = asyncio.get_event_loop()
    fut: asyncio.Future = loop.create_future()
    _l2cap_pending[key] = (fut, loop)
    try:
        peripheral.openL2CAPChannel_(psm)
        return await asyncio.wait_for(fut, timeout=10.0)
    finally:
        _l2cap_pending.pop(key, None)


# --- NSRunLoop thread that drains the L2CAP input stream -----------------

def _stream_thread(in_stream, q: Queue, stop_evt: threading.Event):
    """Runs the NSRunLoop and forwards bytes to the asyncio side."""

    class StreamDelegate(NSObject):
        def stream_handleEvent_(self, stream, event):
            if event == NSStreamEventHasBytesAvailable:
                while stream.hasBytesAvailable():
                    n, data = stream.read_maxLength_(None, 4096)
                    if n <= 0:
                        break
                    q.put(bytes(data[:n]))
            elif event == NSStreamEventOpenCompleted:
                q.put(("__open__",))
            elif event == NSStreamEventEndEncountered:
                q.put(("__end__",))
            elif event == NSStreamEventErrorOccurred:
                q.put(("__error__", str(stream.streamError())))

    delegate = StreamDelegate.alloc().init()
    in_stream.setDelegate_(delegate)
    in_stream.scheduleInRunLoop_forMode_(NSRunLoop.currentRunLoop(),
                                         NSDefaultRunLoopMode)
    in_stream.open()
    rl = NSRunLoop.currentRunLoop()
    while not stop_evt.is_set():
        rl.runMode_beforeDate_(NSDefaultRunLoopMode,
                                NSDate.dateWithTimeIntervalSinceNow_(0.05))
    in_stream.close()


async def main():
    print("scanning for SensaPulse v1...")
    dev = await BleakScanner.find_device_by_name("SensaPulse v1", timeout=8)
    if not dev:
        print("not found"); return 1
    print(f"connecting to {dev.address}")

    async with BleakClient(dev) as client:
        print(f"connected; opening L2CAP CoC PSM 0x{PSM:04x}...")
        channel = await open_l2cap_channel(client, PSM)
        print(f"channel opened psm={channel.PSM()}")

        in_stream = channel.inputStream()
        q: Queue = Queue()
        stop_evt = threading.Event()
        t = threading.Thread(target=_stream_thread,
                             args=(in_stream, q, stop_evt), daemon=True)
        t.start()
        await asyncio.sleep(0.3)  # let stream open

        length = 1024 * 1024
        print(f"firmware will auto-blast {length} byte on L2CAP open...")

        recv_total = 0
        t0 = time.time()
        deadline = t0 + 300.0
        while recv_total < length and time.time() < deadline:
            try:
                item = await asyncio.get_event_loop().run_in_executor(
                    None, q.get, True, 1.0)
            except Empty:
                continue
            if isinstance(item, tuple):
                tag = item[0]
                print(f"  stream event: {tag}")
                if tag == "__error__" or tag == "__end__":
                    break
                continue
            recv_total += len(item)
            if recv_total < 5000 or recv_total // 25000 != (recv_total - len(item)) // 25000:
                elapsed = time.time() - t0
                rate = recv_total / elapsed / 1024 if elapsed > 0 else 0
                print(f"  recv {recv_total} byte ({rate:.1f} KB/s)")

        elapsed = time.time() - t0
        rate = recv_total / elapsed / 1024 if elapsed > 0 else 0
        print(f"FINAL: {recv_total}/{length} byte in {elapsed:.1f}s ({rate:.1f} KB/s)")

        stop_evt.set()
        t.join(timeout=2.0)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
