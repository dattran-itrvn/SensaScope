# PCB v1 — Heart-sound capture & dual-mic ANC feasibility (findings 2026-06-09/10)

Measurement-side conclusions from a multi-recording bench session. Hardware/mechanical
design strategy for v2 belongs with the Cowork agent; this doc is the quantified evidence.

## TL;DR

1. **PDM channels are correct** (body = ch0, ambient = ch1), verified by bench tap-test on
   committed firmware `c3d02c3`. body→ch0, ambient→ch1.
2. **Dual-mic ambient ANC is NOT viable on PCB v1**, and — importantly — **neither source
   paper actually used ANC**. The two papers achieve auscultation with a *single* mic +
   soft acoustic coupling / mic directionality + single-channel DSP.
3. **The body mic is healthy** and captures the **low-frequency core of heart sound
   (20–60 Hz)** — audible as a heartbeat when amplified. Versus real clinical PCGs it has
   **~3× excess <20 Hz motion** and a **~10× deficit in 60–150 Hz** (the "crispness" band).
4. **Mounting matters**: soft/foam-isolated mount captured ~5× more 60–150 Hz than a rigid
   3M-tape skin bond — quantified support for the "soft coupling" thesis (Paper 1).

## Method

- `tools/honest_metrics.py` — replaces the tautological `heart_snr_db` with two non-circular
  measures: (a) inter-mic magnitude-squared coherence by band; (b) cardiac cycle fidelity
  (autocorr-peak σ + inter-beat CV) before/after NLMS.
- Welch PSD band-energy analysis of the body channel (ch0) silence window.
- Comparison against real clinical PCGs from **PhysioNet/CinC Challenge 2016** (ODC-BY,
  2000 Hz): a0001, a0028, a0069. (normal/abnormal labels not individually verified —
  used for band-shape reference only.)

## Why ANC fails here (measured, not assumed)

- Body↔ambient coherence is high in-band across all on-body recordings (heart ~0.6,
  **lung ~0.85–0.92**, reproduced over multiple windows). The ambient channel linearly
  carries the body signal, so an ambient-referenced canceller subtracts the very signal we
  want: inter-beat CV worsens every window while the (tautological) `heart_snr_db` falsely
  shows +1 dB "improvement".
- Off-body in free air, both mics track the room nearly identically (coherence ~1.0 during a
  played tone) → the body mic, being an unsealed/under-coupled MEMS, hears largely the same
  airborne field as the ambient mic.
- A sealed solid (TPU) acoustic chamber + 3M tape **did block airborne** (body 200–500 Hz
  fell 57%→~1% on-body) but did **not** lift the acoustic heart band — confirming sealing is
  not the limiting factor.

## Body channel vs real PCG — band energy (% of total)

Silence window, ch0 body; real PCGs whole-file.

| Recording | 0–20 Hz | 20–60 Hz | 60–150 Hz | 150–300 Hz | 300–500 Hz |
|---|---|---|---|---|---|
| **PCG a0001** (real) | 17 | 70 | 11 | 0.2 | 0 |
| **PCG a0028** (real) | 10 | 73 | 13 | 2.2 | 0.6 |
| **PCG a0069** (real) | 14 | 60 | 22 | 0.7 | 0.1 |
| **real avg** | **~14** | **~68** | **~15** | ~1 | ~0.2 |
| anc_protocol (rigid 3M tape) | 43 | 47 | **1.6** | 0.8 | 0.2 |
| newtape | 62 | 23 | 4.5 | 1.1 | 0.3 |
| board_off_skin (foam-isolated) | 46 | 27 | **8.4** | 3.8 | 1.6 |
| acoustic_chamber (TPU + tape) | 35 | 46 | 4.5 | 1.3 | 0.3 |
| upper_chest | 70 | 21 | 2.1 | 0.4 | 0.1 |

Reading:
- Heart sound is **intrinsically low-frequency** — real PCGs put ~68% in 20–60 Hz, only ~15%
  in 60–150 Hz, ~0 above 150 Hz.
- The device **captures the 20–60 Hz core** (best configs ~46–47%) → why a heartbeat is
  audible. The "heart sound I heard before" = this low-band signal amplified ~100–500×.
- Two consistent gaps: **excess <20 Hz** (motion/seismocardiography, 35–70% vs ~14% real)
  and **deficit 60–150 Hz** (1.6–8.4% vs ~15% real), plus a broadband noise floor >200 Hz.
- **Soft/foam mount (8.4%) ≫ rigid tape (1.6%)** in the 60–150 Hz band.

## Cross-check vs the two inspiration papers

- **Lee et al., Sci. Adv. 2022 (soft wearable stethoscope):** single MEMS mic; "isolate the
  microphone from the core circuit area"; soft elastomer + **silicone gel (300 µm, 4 kPa)**
  acoustic coupling; **wavelet denoising** (single channel). Explicitly shows a *rigid*
  version of their own device produces corrupted data — rigidity is the enemy.
- **Lee et al., Engineering 2025 (LSMP):** **nRF52832 + PDM MEMS + BLE** (≈ our electronics).
  Two mics appear only in a *directionality comparison*; they **select one uni-directional
  mic** "to reduce the influence of external noise". 2 mm acoustic port + 3D-printed elastic
  enclosure. Single-channel DSP (HR: 20–200 Hz; RR: 200–500 Hz). **No ANC.**
- Implication: the dual-mic ambient-ANC premise behind PCB v1's 2-mic architecture is not
  supported by either source. To verify mic directionality for v2, check the
  MP23DB01HP / IMP34DT05 datasheets (likely omnidirectional).

## Quantified targets for v2 (§13 mechanical/acoustic — for Cowork)

1. **Lift 60–150 Hz ~10×** → soft acoustic coupling (silicone gel, Paper 1) instead of an air
   cavity; soft/isolated mic mount instead of rigid 3M skin bond.
2. **Suppress <20 Hz** motion → mechanical isolation reducing chest-wall motion pickup +
   HPF ~20 Hz in DSP.
3. **Lower the >200 Hz noise floor.**
4. **Single mic + single-channel DSP; drop dual-mic ANC.** Consider a uni-directional mic
   (Paper 2) if directionality-based ambient rejection is wanted.

## Caveats

- One NLMS config tested (512 taps, µ=0.1, tuned on lost legacy data); the coherence result
  is config-independent so the ANC conclusion stands regardless.
- PhysioNet a0001/a0028/a0069 normal/abnormal labels not individually confirmed (band-shape
  reference only).
- Acoustic-vs-vibration of the 20–60 Hz content not fully separated; the decisive test
  (block the skin-side acoustic port and re-record) was proposed but not run.

## Artifacts

- Tool: `tools/honest_metrics.py`.
- Recordings (local only, `*.wav` git-ignored): `recordings/20260609_anc_protocol`,
  `…_newtape`, `…_board_off_skin`, `recordings/20260610_acoustic_chamber`, `…_upper_chest`,
  `…_offbody_ambient`. All firmware `c3d02c3` (fixed-era).
