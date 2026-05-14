"""Ablation: does the ambient mic actually contribute to noise cancellation?

Compares 3 pipelines on each posture segment:
  A. Bandpass-only (1-mic baseline)              — body + 20-200 Hz BPF, no NLMS
  B. NLMS with ZEROS (no ambient information)    — sanity check, should ≈ A
  C. NLMS with real ambient + bandpass (full)    — current production pipeline

Δ SNR(C - A) = how much the ambient mic actually buys us.
If small (<2 dB) → single-mic design viable, drop ambient mic from BOM.
If large (>5 dB) → ambient mic is essential for the privacy/voice removal
that we need.
"""
# %% imports + load
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session
from tools.noise_cancel import (heart_filter, heart_snr_db,
                                  nlms_subtract, hr_peak_count)

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00001")
OUT = REPO / "notebooks" / "figures"

s = load_session(SESSION)
audio = s["audio"]; fs = s["fs_audio"]; imu = s["imu"]
body = audio[:, 0].astype(np.float32) / 32768
amb  = audio[:, 1].astype(np.float32) / 32768

# %% IMU segmentation (same as v2_postures.py)
G = 1.0 / 16384.0
gx = signal.sosfiltfilt(signal.butter(2, 0.5, "low", fs=52, output="sos"), imu.ax * G)
gy = signal.sosfiltfilt(signal.butter(2, 0.5, "low", fs=52, output="sos"), imu.ay * G)
gz = signal.sosfiltfilt(signal.butter(2, 0.5, "low", fs=52, output="sos"), imu.az * G)
m = np.sqrt(gx**2 + gy**2 + gz**2)
gx_u, gy_u, gz_u = gx/m, gy/m, gz/m
dot = np.clip(gx_u[1:]*gx_u[:-1] + gy_u[1:]*gy_u[:-1] + gz_u[1:]*gz_u[:-1], -1, 1)
ang_vel = np.concatenate([[0], np.degrees(np.arccos(dot))*52])
ang_vel_s = signal.savgol_filter(ang_vel, 21, 2)
imu_t = (imu.t_us.values - imu.t_us.iloc[0]) / 1e6

is_stable = ang_vel_s < 5.0
runs = []
i, min_len = 0, int(15*52)
while i < len(is_stable):
    if is_stable[i]:
        j = i
        while j < len(is_stable) and is_stable[j]: j += 1
        if j - i >= min_len: runs.append((i, j))
        i = j
    else: i += 1
print(f"posture plateaus: {len(runs)}")

LABELS = ["1 đứng", "2 nằm", "3 nghiêng phải", "4 nghiêng trái", "5 nằm thẳng",
          "6 nghiêng phải", "7 nghiêng trái", "8 nằm thẳng", "9 đứng"]

# %% Run 3 pipelines per segment
print(f"\n{'#':>3}  {'label':<22} {'raw':>7}  {'A bp-only':>10}  {'B NLMS-0':>10}  "
      f"{'C full':>8}  {'ΔC-A':>6}")
results = []
for k, (i0, i1) in enumerate(runs):
    t0_s = imu_t[i0] + 5.0
    t1_s = imu_t[i1 - 1]
    if t1_s - t0_s < 10.0: continue
    label = LABELS[k] if k < len(LABELS) else f"?{k}"
    n0, n1 = int(t0_s * fs), int(t1_s * fs)
    body_seg = body[n0:n1]
    amb_seg  = amb[n0:n1]

    raw_snr = heart_snr_db(body_seg, fs)

    # A: bandpass-only (no NLMS, no ambient)
    A = heart_filter(body_seg, fs=fs)
    snr_A = heart_snr_db(A, fs)

    # B: NLMS with zeros (sanity)
    zeros = np.zeros_like(amb_seg)
    B_nlms = nlms_subtract(zeros, body_seg, n_taps=512, mu=0.1)
    B = heart_filter(B_nlms, fs=fs)
    snr_B = heart_snr_db(B, fs)

    # C: full pipeline (NLMS with real ambient + bandpass)
    C_nlms = nlms_subtract(amb_seg, body_seg, n_taps=512, mu=0.1)
    C = heart_filter(C_nlms, fs=fs)
    snr_C = heart_snr_db(C, fs)

    delta = snr_C - snr_A
    results.append({
        "k": k+1, "label": label, "raw": raw_snr,
        "A": snr_A, "B": snr_B, "C": snr_C, "delta": delta
    })
    print(f"{k+1:>3}  {label:<22} {raw_snr:>+7.2f}  {snr_A:>+10.2f}  "
          f"{snr_B:>+10.2f}  {snr_C:>+8.2f}  {delta:>+6.2f}")

# %% Aggregate
A_arr = np.array([r["A"] for r in results])
B_arr = np.array([r["B"] for r in results])
C_arr = np.array([r["C"] for r in results])
delta_arr = np.array([r["delta"] for r in results])

print(f"\n=== aggregate (across {len(results)} postures) ===")
print(f"A  bandpass-only:        mean {A_arr.mean():+.2f} dB, std {A_arr.std():.2f}")
print(f"B  NLMS(zeros)+bandpass: mean {B_arr.mean():+.2f} dB, std {B_arr.std():.2f} (sanity)")
print(f"C  full pipeline:        mean {C_arr.mean():+.2f} dB, std {C_arr.std():.2f}")
print(f"Δ (C-A): mean {delta_arr.mean():+.2f} dB, range [{delta_arr.min():+.1f}, {delta_arr.max():+.1f}]")
print(f"Δ (B-A): mean {(B_arr-A_arr).mean():+.2f} dB (should be ≈0 — NLMS with zeros is identity)")

# %% Figure — bar chart per posture
fig, axs = plt.subplots(2, 1, figsize=(14, 8))

x = np.arange(len(results))
labels = [r["label"] for r in results]

axs[0].bar(x - 0.27, A_arr, 0.25, label="A: bandpass only", color="steelblue")
axs[0].bar(x       , B_arr, 0.25, label="B: NLMS(zeros) + bp (sanity)",
            color="lightgray")
axs[0].bar(x + 0.27, C_arr, 0.25, label="C: full NLMS(ambient) + bp", color="crimson")
axs[0].set_xticks(x); axs[0].set_xticklabels(labels, rotation=30, ha="right")
axs[0].set_ylabel("heart-band SNR (dB)")
axs[0].set_title("ambient-mic ablation — 3 pipelines per posture")
axs[0].legend(loc="lower right"); axs[0].grid(axis="y", alpha=0.3)
axs[0].axhline(0, color="grey", lw=0.5)

axs[1].bar(x, delta_arr, color=["green" if d >= 3 else "orange" if d >= 1 else "red"
                                  for d in delta_arr],
           edgecolor="black", linewidth=0.4)
for xi, d in zip(x, delta_arr):
    axs[1].text(xi, d + 0.5 if d >= 0 else d - 0.8, f"{d:+.1f}",
                ha="center", va="bottom" if d >= 0 else "top", fontsize=8)
axs[1].set_xticks(x); axs[1].set_xticklabels(labels, rotation=30, ha="right")
axs[1].set_ylabel("Δ SNR  C − A  (dB)")
axs[1].set_title("contribution of ambient mic — green ≥3 dB, orange ≥1 dB, red <1 dB")
axs[1].grid(axis="y", alpha=0.3); axs[1].axhline(0, color="grey", lw=0.5)

plt.tight_layout()
plt.savefig(OUT / "ablation_ambient_mic.png", dpi=110)
plt.close()
print(f"\nsaved {OUT / 'ablation_ambient_mic.png'}")

# %% Verdict
mean_delta = delta_arr.mean()
if mean_delta >= 5:
    verdict = "AMBIENT MIC IS CRITICAL — large SNR gain"
elif mean_delta >= 2:
    verdict = "AMBIENT MIC IS USEFUL — moderate SNR gain"
elif mean_delta >= 0.5:
    verdict = "AMBIENT MIC IS MARGINAL — small SNR gain, single-mic design viable"
else:
    verdict = "AMBIENT MIC PROVIDES NO BENEFIT — drop from BOM"

print(f"\nverdict: {verdict}  (mean Δ = {mean_delta:+.2f} dB)")
