"""SensaPulse session loader.

Loads a session folder produced by SensaPulse v1.0 firmware
(audio.wav, imu.csv, meta.json) into a single dict consumable by
notebooks and the offline ML pipeline:

    load_session(path) → {
        "audio":    np.ndarray (N, 2) int16   — col0=body, col1=ambient
        "fs_audio": int (16000)
        "imu":      pd.DataFrame[t_us,ax,ay,az,gx,gy,gz]
        "fs_imu":   int (52)
        "meta":     dict (parsed meta.json)
    }

    CHANNEL ORDER — read this. Firmware BEFORE the 2026-06-08 PDM L/R fix
    (app/src/audio.c) wrote the two mics SWAPPED: file col0 = ambient,
    col1 = body (proven by bench tap-test, SESSION_00002 vs 00003). Every
    notebook + this loader assume col0=body, so load_session un-swaps
    legacy recordings by default (legacy_channels=True). Recordings from
    fixed firmware are already body-first → pass legacy_channels=False.
    The meta.json hash can NOT distinguish the two (the fix shipped
    uncommitted, same hash), so this is an explicit caller choice.

Why stdlib `wave` instead of `scipy.io.wavfile`? scipy is an ~80 MB
dependency for one read. The stdlib module + numpy.frombuffer
produce the same array for canonical PCM WAV like ours, and
SensaPulse's macOS dev environment doesn't ship scipy by default.
If your env has scipy, swap `_read_wav` — the return shape matches.

CLI:
    python -m tools.load_session <session_path>

prints a one-screen summary (duration, peak/mean per channel, IMU
effective rate, audio↔IMU drift).
"""
from __future__ import annotations

import json
import wave
from pathlib import Path
from typing import Tuple, Union

import numpy as np
import pandas as pd


# Filenames produced by app/src/session.c.
_AUDIO_NAME = "audio.wav"
_IMU_NAME   = "imu.csv"
_META_NAME  = "meta.json"

_IMU_COLS = ["t_us", "ax", "ay", "az", "gx", "gy", "gz"]


def _read_wav(path: Path) -> Tuple[int, np.ndarray]:
    """Read a 16-bit PCM WAV into (sample_rate, ndarray[N, ch] int16)."""
    with wave.open(str(path), "rb") as w:
        sr        = w.getframerate()
        nch       = w.getnchannels()
        sampwidth = w.getsampwidth()
        nframes   = w.getnframes()
        raw       = w.readframes(nframes)
    if sampwidth != 2:
        raise ValueError(
            f"{path}: expected 16-bit PCM, got sampwidth={sampwidth}"
        )
    arr = np.frombuffer(raw, dtype=np.int16).reshape(-1, nch)
    return sr, arr


def load_session(path: Union[Path, str], legacy_channels: bool = True) -> dict:
    """Load a SensaPulse session folder.

    `path` should be the directory holding audio.wav + imu.csv + meta.json.
    Raises FileNotFoundError if any of the three is missing.

    legacy_channels: pre-2026-06-08 firmware wrote the PDM mics swapped
    (file col0=ambient, col1=body). When True (default), the audio columns
    are reversed so the returned array is body-first (col0=body, col1=ambient)
    — what every caller assumes. Set False for recordings made by fixed
    firmware (app/src/audio.c alt_map / NRF_PDM_EDGE_LEFTRISING). See the
    module docstring + the bench tap-test note.
    """
    path = Path(path)
    audio_path = path / _AUDIO_NAME
    imu_path   = path / _IMU_NAME
    meta_path  = path / _META_NAME

    for p in (audio_path, imu_path, meta_path):
        if not p.is_file():
            raise FileNotFoundError(f"{p.name} not found in {path}")

    sr, audio = _read_wav(audio_path)
    if legacy_channels and audio.ndim == 2 and audio.shape[1] == 2:
        # Un-swap the legacy PDM channel order → col0=body, col1=ambient.
        audio = audio[:, ::-1].copy()
    imu  = pd.read_csv(imu_path)
    meta = json.loads(meta_path.read_text())

    if list(imu.columns) != _IMU_COLS:
        raise ValueError(
            f"imu.csv columns mismatch: expected {_IMU_COLS}, "
            f"got {list(imu.columns)}"
        )

    return {
        "audio":    audio,
        "fs_audio": int(sr),
        "imu":      imu,
        "fs_imu":   int(meta.get("fs_imu", 52)),
        "meta":     meta,
    }


def summarize(session: dict) -> str:
    audio: np.ndarray   = session["audio"]
    fs_audio            = session["fs_audio"]
    imu: pd.DataFrame   = session["imu"]
    fs_imu              = session["fs_imu"]
    meta                = session["meta"]

    duration_s = audio.shape[0] / fs_audio if fs_audio else float("nan")

    t_us = imu["t_us"].to_numpy()
    if len(t_us) >= 2:
        imu_span_s = (t_us[-1] - t_us[0]) / 1e6
        fs_eff = (len(t_us) - 1) / imu_span_s if imu_span_s > 0 else float("nan")
    else:
        imu_span_s = 0.0
        fs_eff = float("nan")

    drift_ms = (imu_span_s - duration_s) * 1000

    if audio.size:
        pl = int(np.abs(audio[:, 0]).max())
        pr = int(np.abs(audio[:, 1]).max())
        ml = int(audio[:, 0].mean())
        mr = int(audio[:, 1].mean())
    else:
        pl = pr = ml = mr = 0

    lines = [
        f"session id={meta.get('session_id', '?')} "
        f"fw={meta.get('fw_version', '?')}+{meta.get('fw_build_hash', '?')} "
        f"chip={meta.get('chip_id', '?')} "
        f"batt_start={meta.get('batt_mv_start', '?')} mV",
        f"  audio: {audio.shape[0]} pairs × 2 ch = "
        f"{duration_s:.2f} s @ {fs_audio} Hz (dtype={audio.dtype})",
        f"         peak ch0(body)={pl}  ch1(ambient)={pr}    "
        f"mean ch0={ml}  ch1={mr}",
        f"  imu:   {len(imu)} rows over {imu_span_s:.2f} s, "
        f"fs_eff={fs_eff:.2f} Hz (nominal {fs_imu} Hz)",
        f"         drift = audio - imu = {-drift_ms:+.1f} ms "
        f"(positive = audio longer)",
    ]
    return "\n".join(lines)


def main() -> int:
    import sys
    if len(sys.argv) != 2:
        print("usage: python -m tools.load_session <session_path>",
              file=sys.stderr)
        return 2
    try:
        session = load_session(sys.argv[1])
    except (FileNotFoundError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(summarize(session))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
