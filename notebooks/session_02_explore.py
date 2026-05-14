"""Session 02 explore — first-look at heart/lung recording.

Run as a script: `python3 notebooks/session_02_explore.py`
Or open as a notebook in VSCode/Cursor (Jupytext-style `# %%` cells).

Outputs PNGs to notebooks/figures/.
"""
# %% imports + load
from __future__ import annotations

import json
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy import signal

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00002")
OUT = REPO / "notebooks" / "figures"
OUT.mkdir(parents=True, exist_ok=True)

s = load_session(SESSION)
audio = s["audio"]                # (N, 2) int16
fs    = s["fs_audio"]             # 16000
imu   = s["imu"]                  # DataFrame t_us, ax..gz
meta  = s["meta"]

body    = audio[:, 0].astype(np.float32) / 32768.0
ambient = audio[:, 1].astype(np.float32) / 32768.0
N = len(body)
t_audio = np.arange(N) / fs       # seconds from session start

print(f"session_id={meta['session_id']}  fs={fs}  N={N}  duration={N/fs:.2f}s")
print(f"audio peak: body={np.abs(body).max():.3f}  ambient={np.abs(ambient).max():.3f}")
print(f"imu rows={len(imu)}  span={(imu.t_us.iloc[-1]-imu.t_us.iloc[0])/1e6:.2f}s")

# %% Figure 1 — full waveform overview (decimated)
def decim(x, target=10000):
    step = max(1, len(x) // target)
    # peak envelope (max abs over each bin) preserves visual peaks
    n = len(x) // step * step
    return np.abs(x[:n]).reshape(-1, step).max(axis=1)

env_body    = decim(body)
env_ambient = decim(ambient)
t_env       = np.linspace(0, N/fs, len(env_body))

fig, ax = plt.subplots(2, 1, figsize=(12, 5), sharex=True)
ax[0].plot(t_env, env_body, lw=0.5, color="steelblue")
ax[0].set_title(f"SESSION_{meta['session_id']:05d} body mic — peak envelope (10000-point decim)")
ax[0].set_ylabel("|sample|")
ax[0].set_ylim(0, max(env_body.max(), 0.05) * 1.1)
ax[1].plot(t_env, env_ambient, lw=0.5, color="darkorange")
ax[1].set_title("ambient mic — peak envelope")
ax[1].set_xlabel("time (s)")
ax[1].set_ylabel("|sample|")
ax[1].set_ylim(0, max(env_ambient.max(), 0.05) * 1.1)
plt.tight_layout()
plt.savefig(OUT / "01_waveform_overview.png", dpi=110)
plt.close()
print(f"saved {OUT / '01_waveform_overview.png'}")

# %% Figure 2 — spectrogram 0-2000 Hz (heart/lung relevant band)
nperseg = 2048      # 128 ms window → freq res ~8 Hz
noverlap = 1024

fig, ax = plt.subplots(2, 1, figsize=(12, 6), sharex=True)
for i, (ch, name, color) in enumerate([(body, "body", "viridis"),
                                       (ambient, "ambient", "inferno")]):
    f, t, Sxx = signal.spectrogram(ch, fs=fs, nperseg=nperseg, noverlap=noverlap)
    mask = f <= 2000
    # log power, clipped to avoid -inf
    p = 10 * np.log10(np.maximum(Sxx[mask], 1e-12))
    pcm = ax[i].pcolormesh(t, f[mask], p, shading="auto", cmap=color,
                            vmin=p.max() - 60, vmax=p.max())
    ax[i].set_ylabel("freq (Hz)")
    ax[i].set_title(f"{name} spectrogram (0-2000 Hz)")
    plt.colorbar(pcm, ax=ax[i], label="dB")
ax[1].set_xlabel("time (s)")
plt.tight_layout()
plt.savefig(OUT / "02_spectrogram.png", dpi=110)
plt.close()
print(f"saved {OUT / '02_spectrogram.png'}")

# %% Figure 3 — 5-second zoom at middle of session (clean segment for heart)
t_mid = N // 2 - int(2.5 * fs)
seg_body = body[t_mid : t_mid + int(5 * fs)]
seg_amb  = ambient[t_mid : t_mid + int(5 * fs)]
seg_t    = np.arange(len(seg_body)) / fs

# also compute envelope of band-passed (heart band: 20-200 Hz) for S1/S2 visibility
sos = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")
body_hb  = signal.sosfiltfilt(sos, seg_body)
env_hb   = np.abs(signal.hilbert(body_hb))
# smooth envelope
env_hb_s = signal.savgol_filter(env_hb, 401, 3)

fig, ax = plt.subplots(3, 1, figsize=(12, 7), sharex=True)
ax[0].plot(seg_t, seg_body, lw=0.4, color="steelblue")
ax[0].set_title(f"body mic — 5 s zoom @ t={t_mid/fs:.1f}s")
ax[0].set_ylabel("amp (norm)")
ax[1].plot(seg_t, body_hb, lw=0.4, color="navy", label="band-pass 20-200 Hz")
ax[1].plot(seg_t, env_hb_s, lw=1.5, color="crimson", label="Hilbert envelope (smoothed)")
ax[1].set_ylabel("amp")
ax[1].legend(loc="upper right")
ax[1].set_title("heart band envelope — S1 / S2 peaks should be visible here")
ax[2].plot(seg_t, seg_amb, lw=0.4, color="darkorange")
ax[2].set_title("ambient mic — same window")
ax[2].set_xlabel("time (s)")
ax[2].set_ylabel("amp (norm)")
plt.tight_layout()
plt.savefig(OUT / "03_zoom_5sec.png", dpi=110)
plt.close()
print(f"saved {OUT / '03_zoom_5sec.png'}")

# %% Figure 4 — heart-rate estimate via autocorrelation on the envelope
# Use a longer middle window (30 sec) and compute autocorrelation of the
# heart-band envelope. Peak between 0.4-1.5 s lag → IBI → BPM.
seg_len = int(30 * fs)
t0 = N // 2 - seg_len // 2
seg = body[t0 : t0 + seg_len]
seg_hb = signal.sosfiltfilt(sos, seg)
env = np.abs(signal.hilbert(seg_hb))
env -= env.mean()
ac = signal.correlate(env, env, mode="full")
ac = ac[len(ac) // 2 :]
lags = np.arange(len(ac)) / fs
# search 0.4-1.5 s (40-150 BPM range)
search_lo = int(0.4 * fs)
search_hi = int(1.5 * fs)
peak_idx = search_lo + int(np.argmax(ac[search_lo:search_hi]))
ibi = peak_idx / fs
bpm = 60.0 / ibi

fig, ax = plt.subplots(1, 1, figsize=(10, 4))
ax.plot(lags[: search_hi + int(0.2 * fs)], ac[: search_hi + int(0.2 * fs)])
ax.axvline(ibi, color="red", linestyle="--",
           label=f"peak at {ibi*1000:.0f} ms → ~{bpm:.1f} BPM")
ax.set_xlabel("lag (s)")
ax.set_ylabel("autocorrelation")
ax.set_title("heart-rate estimate via envelope autocorrelation (30 s middle window)")
ax.legend()
plt.tight_layout()
plt.savefig(OUT / "04_heart_rate.png", dpi=110)
plt.close()
print(f"saved {OUT / '04_heart_rate.png'}  → estimated HR ≈ {bpm:.1f} BPM")

# %% Figure 5 — IMU
imu_t = (imu.t_us - imu.t_us.iloc[0]) / 1e6  # seconds from session start
g_per_lsb = 1.0 / 16384.0    # ±2g range, 16-bit signed
dps_per_lsb = 1.0 / 131.2    # ±250 dps range, default Gyro setup

ax_g = imu.ax * g_per_lsb
ay_g = imu.ay * g_per_lsb
az_g = imu.az * g_per_lsb
mag_g = np.sqrt(ax_g**2 + ay_g**2 + az_g**2)

fig, ax = plt.subplots(3, 1, figsize=(12, 7), sharex=True)
ax[0].plot(imu_t, ax_g, label="ax", lw=0.7)
ax[0].plot(imu_t, ay_g, label="ay", lw=0.7)
ax[0].plot(imu_t, az_g, label="az", lw=0.7)
ax[0].axhline(1, color="grey", ls=":", lw=0.5)
ax[0].axhline(-1, color="grey", ls=":", lw=0.5)
ax[0].legend(loc="upper right")
ax[0].set_ylabel("accel (g)")
ax[0].set_title("IMU accelerometer — orientation + motion")

ax[1].plot(imu_t, mag_g, color="purple", lw=0.7)
ax[1].axhline(1, color="grey", ls=":", lw=0.5)
ax[1].set_ylabel("|accel| (g)")
ax[1].set_title("accel magnitude — should sit near 1 g when stationary, spikes = motion")

ax[2].plot(imu_t, imu.gx * dps_per_lsb, label="gx", lw=0.7)
ax[2].plot(imu_t, imu.gy * dps_per_lsb, label="gy", lw=0.7)
ax[2].plot(imu_t, imu.gz * dps_per_lsb, label="gz", lw=0.7)
ax[2].legend(loc="upper right")
ax[2].set_ylabel("gyro (dps)")
ax[2].set_xlabel("time (s)")
ax[2].set_title("gyro — angular velocity")
plt.tight_layout()
plt.savefig(OUT / "05_imu.png", dpi=110)
plt.close()
print(f"saved {OUT / '05_imu.png'}")

# %% summary stats for posture
gravity_axis = np.array([ax_g.mean(), ay_g.mean(), az_g.mean()])
gravity_mag  = np.linalg.norm(gravity_axis)
print()
print("=== posture proxy (mean accel vector) ===")
print(f"  vector: ({gravity_axis[0]:.2f}, {gravity_axis[1]:.2f}, {gravity_axis[2]:.2f}) g")
print(f"  magnitude: {gravity_mag:.3f} g (expect ~1.0)")
# rough tilt: angle between gravity vector and Z axis
tilt_deg = np.degrees(np.arccos(gravity_axis[2] / gravity_mag))
print(f"  tilt from Z (vertical) axis: {tilt_deg:.1f}°")
print(f"  → dominant axis: {'X' if abs(gravity_axis[0])>0.7 else 'Y' if abs(gravity_axis[1])>0.7 else 'Z'}")

motion_idx = mag_g.std()
print(f"  motion stdev (over session): {motion_idx*1000:.1f} mg")
print(f"  → {'stationary' if motion_idx<0.05 else 'some motion'}")
