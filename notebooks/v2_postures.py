"""v2 robustness — pipeline performance across 10 postures.

User recording sequence (2026-05-14):
  1 đứng, 2 nằm, 3 nghiêng phải, 4 nghiêng trái, 5 nằm thẳng,
  6 nghiêng phải, 7 nghiêng trái, 8 nằm thẳng, 9 đứng, 10 ngồi

Detect posture transitions automatically by tracking gravity vector
direction in the IMU stream (change points = high angular velocity),
then map plateaus to the labelled sequence in order. Run NLMS
noise-cancel pipeline on each segment, report SNR, HR, BPM.
"""
# %% imports + load
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy import signal

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session
from tools.noise_cancel import (pipeline, heart_snr_db, hr_periodicity,
                                  hr_robust, hr_robust_two_pass, hr_smooth_across)

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00001")
OUT = REPO / "notebooks" / "figures"
OUT.mkdir(parents=True, exist_ok=True)

LABELS = [
    "1 đứng", "2 nằm", "3 nghiêng phải", "4 nghiêng trái", "5 nằm thẳng",
    "6 nghiêng phải", "7 nghiêng trái", "8 nằm thẳng", "9 đứng", "10 ngồi",
]

s = load_session(SESSION)
audio = s["audio"]
fs    = s["fs_audio"]
imu   = s["imu"]
print(f"audio: {len(audio)/fs:.1f}s, IMU: {len(imu)} rows over {(imu.t_us.iloc[-1]-imu.t_us.iloc[0])/1e6:.1f}s")

# %% Posture segmentation via IMU gravity vector
G_LSB = 1.0 / 16384.0     # ±2g range
ax = imu.ax.values * G_LSB
ay = imu.ay.values * G_LSB
az = imu.az.values * G_LSB
imu_t = (imu.t_us.values - imu.t_us.iloc[0]) / 1e6   # seconds from session start

# Low-pass to estimate gravity (remove motion / heart pulse / vibration)
fs_imu = 52
sos_lp = signal.butter(2, 0.5, btype="low", fs=fs_imu, output="sos")
gx = signal.sosfiltfilt(sos_lp, ax)
gy = signal.sosfiltfilt(sos_lp, ay)
gz = signal.sosfiltfilt(sos_lp, az)
g_mag = np.sqrt(gx**2 + gy**2 + gz**2)
gx_u, gy_u, gz_u = gx / g_mag, gy / g_mag, gz / g_mag

# Angular velocity of the gravity unit vector — high = posture changing
# Compute angle between consecutive samples; smooth.
dot = np.clip(gx_u[1:] * gx_u[:-1] + gy_u[1:] * gy_u[:-1] + gz_u[1:] * gz_u[:-1],
              -1, 1)
ang_vel = np.degrees(np.arccos(dot)) * fs_imu        # deg/s
# pad to match length
ang_vel = np.concatenate([[0], ang_vel])
ang_vel_smooth = signal.savgol_filter(ang_vel, 21, 2)

# Plateau detector: low angular velocity for ≥ MIN_PLATEAU_S → stable posture
MIN_PLATEAU_S = 15.0
ANG_THRESHOLD = 5.0   # deg/s

is_stable = ang_vel_smooth < ANG_THRESHOLD
# Find runs of "stable" of length >= MIN_PLATEAU_S × fs_imu
min_len = int(MIN_PLATEAU_S * fs_imu)
runs = []
i = 0
while i < len(is_stable):
    if is_stable[i]:
        j = i
        while j < len(is_stable) and is_stable[j]:
            j += 1
        if j - i >= min_len:
            runs.append((i, j))
        i = j
    else:
        i += 1

print(f"\nIMU plateaus detected: {len(runs)} (expected {len(LABELS)})")
for k, (i0, i1) in enumerate(runs):
    t0, t1 = imu_t[i0], imu_t[i1 - 1]
    gv = (np.median(gx_u[i0:i1]), np.median(gy_u[i0:i1]), np.median(gz_u[i0:i1]))
    print(f"  plateau {k+1}: {t0:5.1f}–{t1:5.1f}s  Δ={t1-t0:5.1f}s  "
          f"gravity≈({gv[0]:+.2f},{gv[1]:+.2f},{gv[2]:+.2f})")

# %% Figure 1 — IMU posture timeline
fig, axs = plt.subplots(3, 1, figsize=(13, 8), sharex=True)
axs[0].plot(imu_t, gx_u, label="gx", lw=0.8, color="C0")
axs[0].plot(imu_t, gy_u, label="gy", lw=0.8, color="C1")
axs[0].plot(imu_t, gz_u, label="gz", lw=0.8, color="C2")
axs[0].legend(loc="upper right")
axs[0].set_ylabel("gravity unit vector")
axs[0].set_title("IMU-derived gravity direction (low-passed accel)")

axs[1].plot(imu_t, ang_vel_smooth, color="purple", lw=0.7)
axs[1].axhline(ANG_THRESHOLD, color="red", linestyle="--", label=f"plateau threshold {ANG_THRESHOLD} deg/s")
axs[1].legend(loc="upper right")
axs[1].set_ylabel("ang vel (deg/s)")
axs[1].set_title("posture-transition activity (low = stable, high = changing)")
axs[1].set_yscale("symlog", linthresh=1)

# Plot plateau bands + labels
for k, (i0, i1) in enumerate(runs):
    color = plt.cm.tab10(k % 10)
    label = LABELS[k] if k < len(LABELS) else f"?{k}"
    for ax in axs:
        ax.axvspan(imu_t[i0], imu_t[i1 - 1], alpha=0.15, color=color)
    axs[2].axvspan(imu_t[i0], imu_t[i1 - 1], alpha=0.3, color=color,
                   label=label)

# Audio overview as bottom strip
body = audio[:, 0].astype(np.float32) / 32768.0
amb  = audio[:, 1].astype(np.float32) / 32768.0
N = len(body)
step = max(1, N // 5000)
n = (N // step) * step
env_body = np.abs(body[:n]).reshape(-1, step).max(axis=1)
t_env = np.linspace(0, N/fs, len(env_body))
axs[2].plot(t_env, env_body, lw=0.4, color="steelblue")
axs[2].set_ylabel("body |amp|")
axs[2].set_xlabel("time (s)")
axs[2].set_title("body mic envelope + posture segments")
axs[2].legend(loc="upper right", fontsize=7, ncol=2)
plt.tight_layout()
plt.savefig(OUT / "p1_posture_timeline.png", dpi=110)
plt.close()
print(f"saved {OUT / 'p1_posture_timeline.png'}")

# %% Run pipeline per segment + collect metrics
if len(runs) != len(LABELS):
    print(f"\nWARNING: plateau count {len(runs)} != label count {len(LABELS)}. "
          f"Labels may mis-align. Continuing with available plateaus.")

# Build per-segment cleaned signals + metrics (pass-0 — just NLMS + SNR)
results = []
cleaned_segs = []
for k, (i0_imu, i1_imu) in enumerate(runs):
    label = LABELS[k] if k < len(LABELS) else f"?{k}"
    t0_s, t1_s = imu_t[i0_imu], imu_t[i1_imu - 1]
    t0_pad = t0_s + 5.0      # skip first 5 s for NLMS convergence
    if t1_s - t0_pad < 10.0:
        continue
    n0, n1 = int(t0_pad * fs), int(t1_s * fs)
    body_seg = body[n0:n1]
    amb_seg  = amb[n0:n1]
    raw_snr = heart_snr_db(body_seg, fs)
    clean = pipeline(body_seg, amb_seg, fs=fs, n_taps=512, mu=0.1)
    clean_snr = heart_snr_db(clean, fs)
    ac_sigma, ac_bpm = hr_periodicity(clean, fs)
    results.append({
        "k": k+1, "label": label, "t0": t0_s, "t1": t1_s,
        "sec": t1_s - t0_s, "raw_snr": raw_snr,
        "clean_snr": clean_snr, "d_snr": clean_snr - raw_snr,
        "ac_sigma": ac_sigma, "ac_bpm": ac_bpm,
    })
    cleaned_segs.append(clean)

# Two-pass HR estimator on all cleaned segments (uses subject-specific prior)
hr_p1 = [hr_robust(seg, fs) for seg in cleaned_segs]   # pass-1 wide
hr_p2 = hr_robust_two_pass(cleaned_segs, fs)            # pass-2 prior-narrowed

# Smoothing across segments to fill any remaining outliers
p2_estimates = [(b, c) for (b, c, _) in hr_p2]
final_bpms = hr_smooth_across(p2_estimates, max_jump_bpm=20)

print("\n=== per-posture pipeline metrics ===")
print(f"{'#':>3}  {'label':<22} {'sec':>6}  {'raw SNR':>8}  {'NLMS SNR':>8}  {'Δ SNR':>6}  "
      f"{'P1 BPM':>7} {'σ':>5}  {'P2 BPM':>7} {'σ':>5}  {'final':>6}  tag")
for r, p1, p2, fin in zip(results, hr_p1, hr_p2, final_bpms):
    r["p1_bpm"], r["p1_conf"], _      = p1
    r["p2_bpm"], r["p2_conf"], r["p2_tag"] = p2
    r["final_bpm"] = fin
    print(f"{r['k']:>3}  {r['label']:<22} {r['sec']:>6.1f}  {r['raw_snr']:>+8.2f}  "
          f"{r['clean_snr']:>+8.2f}  {r['d_snr']:>+6.2f}  "
          f"{r['p1_bpm']:>7.1f} {r['p1_conf']:>5.2f}  "
          f"{r['p2_bpm']:>7.1f} {r['p2_conf']:>5.2f}  "
          f"{fin:>6.1f}  {r['p2_tag']}")

# %% Figure 2 — bar chart SNR per posture
if results:
    fig, ax = plt.subplots(1, 1, figsize=(13, 5))
    x = np.arange(len(results))
    raw = [r["raw_snr"] for r in results]
    cleaned = [r["clean_snr"] for r in results]
    labels = [r["label"] for r in results]
    ax.bar(x - 0.2, raw, 0.4, label="raw body", color="steelblue")
    ax.bar(x + 0.2, cleaned, 0.4, label="NLMS+bandpass", color="crimson")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=30, ha="right")
    ax.set_ylabel("heart-band SNR (dB)")
    ax.set_title("noise-cancel pipeline performance across 10 postures")
    ax.legend(loc="upper left")
    ax.axhline(0, color="grey", lw=0.5)
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    plt.savefig(OUT / "p2_snr_per_posture.png", dpi=110)
    plt.close()
    print(f"\nsaved {OUT / 'p2_snr_per_posture.png'}")

# %% Figure 3 — HR estimate per posture (3 estimators side-by-side)
if results:
    fig, ax = plt.subplots(1, 1, figsize=(13, 5))
    x = np.arange(len(results))
    labels = [r["label"] for r in results]
    ac_bpms    = [r["ac_bpm"]    for r in results]
    p1_bpms    = [r["p1_bpm"]    for r in results]
    final_bpms = [r["final_bpm"] for r in results]
    p2_confs   = [r["p2_conf"]   for r in results]

    ax.bar(x - 0.27, ac_bpms,    0.25,
            label="autocorr (legacy, ambiguous)",
            color="lightsteelblue", edgecolor="black", linewidth=0.4)
    ax.bar(x       , p1_bpms,    0.25,
            label="spectrum pass-1 (wide search)",
            color="steelblue", edgecolor="black", linewidth=0.4)
    ax.bar(x + 0.27, final_bpms, 0.25,
            label="2-pass + subject prior + cross-smooth (final)",
            color="crimson", edgecolor="black", linewidth=0.4)

    for xi, b, c in zip(x, final_bpms, p2_confs):
        ax.text(xi + 0.27, b + 2, f"σ={c:.1f}", ha="center", va="bottom",
                fontsize=7, color="darkred")

    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=30, ha="right")
    ax.set_ylabel("BPM")
    ax.set_title("HR estimate per posture — three estimators")
    ax.legend(loc="upper left", fontsize=9)
    ax.grid(axis="y", alpha=0.3)
    ax.set_ylim(0, max(max(ac_bpms), max(p1_bpms), max(final_bpms)) * 1.15)
    plt.tight_layout()
    plt.savefig(OUT / "p3_hr_per_posture.png", dpi=110)
    plt.close()
    print(f"saved {OUT / 'p3_hr_per_posture.png'}")

print("\n=== summary ===")
if results:
    d_snrs = [r["d_snr"] for r in results]
    print(f"Δ SNR range: {min(d_snrs):+.1f} to {max(d_snrs):+.1f} dB, "
          f"mean {np.mean(d_snrs):+.1f} dB")
    print(f"AC BPM range (legacy):    {min(r['ac_bpm'] for r in results):.0f}-"
          f"{max(r['ac_bpm'] for r in results):.0f}")
    print(f"P1 BPM range (wide spec): {min(r['p1_bpm'] for r in results):.0f}-"
          f"{max(r['p1_bpm'] for r in results):.0f}")
    final = [r["final_bpm"] for r in results]
    print(f"Final BPM range:          {min(final):.0f}-{max(final):.0f}, "
          f"std {np.std(final):.1f}")
    p2_conf = [r["p2_conf"] for r in results]
    print(f"P2 confidence: mean {np.mean(p2_conf):.1f}σ, min {min(p2_conf):.1f}σ")
