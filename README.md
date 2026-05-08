# SensaPulse

Wearable continuous-wear stethoscope. Two PDM microphones (body + ambient), 6-axis IMU for activity classification, micro-SD logging, BLE for PC sync. Built on Nordic nRF52840 (Ebyte E73-2G4M08S1C module).

This repo contains the device firmware (Zephyr / nRF Connect SDK), a custom-board definition for PCB v1.0, supporting Python tools, and overnight test playbooks.

## Status

| | v1.0 | v1.1 |
|---|---|---|
| Peripheral bring-up | ✅ done | — |
| Streaming recorder (PDM → SD) | ✅ done | — |
| Double-tap toggle | ✅ done | — |
| BLE smoke advertise | ✅ done | — |
| Session manager + 10-min rotation | 🚧 in progress | — |
| IMU CSV + meta.json | 🚧 planned | — |
| LED state machine | 🚧 planned | — |
| BLE sync service (GATT) | — | 🚧 planned |
| PC sync CLI (Python) | — | 🚧 stub |

See `CLAUDE.md` for the canonical project context, including hardware pin map, PCB v1.0 quirks, locked-in firmware spec decisions, and the build/flash/RTT debug workflow. See `docs/SYNC_PROTOCOL.md` for the v1.1 BLE protocol.

## Quick start

```bash
# Build (requires nRF Connect SDK v2.9.3 toolchain in PATH)
cd app
west build -b sensapulse_v1/nrf52840 -p auto -- -DBOARD_ROOT=$(pwd)/..

# Flash (requires J-Link)
west flash

# Capture RTT log
JLinkRTTLogger -Device NRF52840_XXAA -If SWD -Speed 4000 -RTTChannel 0 logs/rtt.log
```

For the full toolchain installation walk-through see `docs/SETUP_MACOS.md`. For migrating to a Claude Code workflow see `MIGRATION.md`.

## Repository layout

```
.
├── CLAUDE.md                          # project context (read this first)
├── MIGRATION.md                       # Cowork ↔ Claude Code workflow
├── README.md
├── LICENSE
├── .claude/
│   └── settings.json                  # Claude Code permission allowlist
├── app/                               # firmware
│   ├── CMakeLists.txt
│   ├── prj.conf
│   ├── boards/sensapulse_v1.overlay   # peripheral DT enables
│   └── src/                           # main.c + per-peripheral modules
├── boards/itrvn/sensapulse_v1/        # Zephyr custom board definition
├── docs/
│   ├── HARDWARE.md                    # pin map, schematic refs
│   ├── SETUP_MACOS.md                 # toolchain installation
│   ├── SYNC_PROTOCOL.md               # v1.1 BLE protocol spec
│   └── PRODUCTION_TODO.md             # hardening checklist (security, etc.)
├── scripts/
│   ├── build_flash.sh
│   ├── rtt_capture.sh
│   ├── parse_rtt.py
│   └── run_loop.py                    # overnight playbook driver
├── playbooks/                         # overnight test scenarios (JSON)
├── tools/
│   └── sync.py                        # PC-side BLE sync CLI (v1.1)
├── logs/                              # per-build logs (gitignored)
└── runs/                              # overnight run reports (gitignored)
```

## Hardware

- MCU: Ebyte E73-2G4M08S1C (Nordic nRF52840)
- Body mic: ST MP23DB01HPTR (PDM, top-port)
- Ambient mic: ST IMP34DT05TR (PDM, bottom-port)
- IMU: ST LSM6DSLTR (6-axis, I²C)
- Storage: micro-SD (SPI)
- Power: Li-ion + TPS7A0333 LDO
- Schematic version: PCB v1.0 dated 2026-04-09

## License

See `LICENSE`.

## Maintainer

Dat Tran, ITRVN. dattran@itrvn.com
