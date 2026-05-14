"""Lung-sound recording scan — find structured-protocol phases in SESSION_00002.

User said the structured 6-phase protocol was done at the very end. We
scan the last 6 min looking for signatures:
  - Phase 1 normal breath: low/mid amplitude
  - Phase 2 deep breath: large slow envelope swings
  - Phase 3 BREATH-HOLD 10-15s: low lung-band energy, heart visible
  - Phase 4 hyperventilation: dense breath cycles
  - Phase 5 cough: sharp brief transients
  - Phase 6 speak: energy in 300-3000 Hz voice band

Plot:
  (a) Full session envelope (lung band 60-2000 Hz)
  (b) Spectrogram 0-3 kHz to see voice/cough
  (c) Voice-band energy (300-3000 Hz) for phase 6 detection
  (d) IMU motion magnitude (for chest movement during breath)
"""
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00002")
OUT = REPO / "notebooks" / "figures"

s = load_session(SESSION)
audio = s["audio"]; fs = s["fs_audio"]; imu = s["imu"]
body = audio[:, 0].astype(np.float32) / 32768
amb  = audio[:, 1].astype(np.float32) / 32768
N = len(body); duration = N / fs
print(f"SESSION_00002: {duration:.1f}s = {duration/60:.2f} min")

# %% Energy bands
sos_lung = signal.butter(4, [60, 2000], btype="band", fs=fs, output="sos")
sos_voice = signal.butter(4, [300, 3000], btype="band", fs=fs, output="sos")
sos_heart = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")

# Smoothed RMS in 250 ms windows
WIN = int(0.25 * fs)
def windowed_rms(x):
    bp = signal.sosfiltfilt(x[0], x[1])
    sq = bp ** 2
    n = (len(sq) // WIN) * WIN
    return np.sqrt(sq[:n].reshape(-1, WIN).mean(axis=1))

body_lung_rms  = windowed_rms((sos_lung, body))
body_voice_rms = windowed_rms((sos_voice, body))
body_heart_rms = windowed_rms((sos_heart, body))
amb_voice_rms  = windowed_rms((sos_voice, amb))
t_win = np.arange(len(body_lung_rms)) * (WIN / fs)

# %% Figure
fig, axs = plt.subplots(5, 1, figsize=(15, 11), sharex=True)

# (a) lung band envelope
axs[0].plot(t_win, body_lung_rms, lw=0.7, color="steelblue")
axs[0].set_ylabel("body 60-2000 Hz\nRMS (250 ms)")
axs[0].set_title("body mic — lung-band envelope")

# (b) heart band envelope (independent of breath)
axs[1].plot(t_win, body_heart_rms, lw=0.7, color="crimson")
axs[1].set_ylabel("body 20-200 Hz\nRMS")
axs[1].set_title("body mic — heart-band envelope (should stay continuous, even during breath hold)")

# (c) voice band on body + ambient
axs[2].plot(t_win, body_voice_rms, lw=0.7, color="darkorange", label="body voice (300-3000 Hz)")
axs[2].plot(t_win, amb_voice_rms,  lw=0.7, color="purple", alpha=0.6, label="ambient voice")
axs[2].set_ylabel("voice-band RMS")
axs[2].legend(loc="upper right")
axs[2].set_title("voice-band activity — speech / cough / external sounds")

# (d) spectrogram body 0-3 kHz
f_, t_, Sxx_ = signal.spectrogram(body, fs=fs, nperseg=4096, noverlap=2048)
mask = f_ <= 3000
p = 10 * np.log10(np.maximum(Sxx_[mask], 1e-12))
pcm = axs[3].pcolormesh(t_, f_[mask], p, shading="auto", cmap="viridis",
                          vmin=p.max() - 60, vmax=p.max())
axs[3].set_ylabel("freq (Hz)")
axs[3].set_title("body mic spectrogram (0-3 kHz) — voice 300-3000, cough transients across all")
plt.colorbar(pcm, ax=axs[3], label="dB")

# (e) IMU motion (chest movement = breath)
imu_t = (imu.t_us.values - imu.t_us.iloc[0]) / 1e6
gyro_mag = np.sqrt(imu.gx**2 + imu.gy**2 + imu.gz**2) / 131.2  # dps
accel_mag = np.sqrt(imu.ax**2 + imu.ay**2 + imu.az**2) / 16384.0
axs[4].plot(imu_t, accel_mag, lw=0.5, color="green", label="|accel| (g)")
axs[4].axhline(1.0, color="grey", ls=":", lw=0.5)
axs[4].set_ylabel("|accel| (g)")
axs[4].set_xlabel("time (s)")
axs[4].set_title("IMU accel magnitude — should be ~1 g when still, spikes = motion")
axs[4].legend(loc="upper right")
axs[4].set_xlim(0, duration)

plt.tight_layout()
plt.savefig(OUT / "lung_scan_full.png", dpi=110)
plt.close()
print(f"saved {OUT / 'lung_scan_full.png'}")

# %% Zoom on last 6 min (structured protocol region)
last_t = duration
first_t = max(0, last_t - 360)
zoom_idx_win = (t_win >= first_t) & (t_win <= last_t)
zoom_idx_imu = (imu_t >= first_t) & (imu_t <= last_t)

fig, axs = plt.subplots(4, 1, figsize=(15, 9), sharex=True)

axs[0].plot(t_win[zoom_idx_win], body_lung_rms[zoom_idx_win], lw=0.8, color="steelblue")
axs[0].set_ylabel("lung band RMS")
axs[0].set_title(f"ZOOM last {min(360, last_t):.0f}s — structured protocol")

axs[1].plot(t_win[zoom_idx_win], body_heart_rms[zoom_idx_win], lw=0.8, color="crimson")
axs[1].set_ylabel("heart band RMS")
axs[1].set_title("heart band — visible even during breath-hold (phase 3)")

axs[2].plot(t_win[zoom_idx_win], body_voice_rms[zoom_idx_win], lw=0.8,
             color="darkorange", label="body")
axs[2].plot(t_win[zoom_idx_win], amb_voice_rms[zoom_idx_win], lw=0.8,
             color="purple", alpha=0.6, label="ambient")
axs[2].set_ylabel("voice band RMS")
axs[2].legend(loc="upper right")
axs[2].set_title("voice/cough events — sharp spikes = cough, sustained = speech")

axs[3].plot(imu_t[zoom_idx_imu], accel_mag[zoom_idx_imu], lw=0.4, color="green")
axs[3].axhline(1.0, color="grey", ls=":", lw=0.5)
axs[3].set_xlabel("time (s)")
axs[3].set_ylabel("|accel| (g)")
axs[3].set_title("IMU — motion from breathing/postural shifts")
axs[3].set_xlim(first_t, last_t)

plt.tight_layout()
plt.savefig(OUT / "lung_scan_last6min.png", dpi=110)
plt.close()
print(f"saved {OUT / 'lung_scan_last6min.png'}")

# %% Detect probable cough events (sharp transients in lung-band envelope)
# Cough = sharp brief spike → derivative of RMS goes high
deriv = np.diff(body_lung_rms)
deriv_thresh = np.std(deriv) * 4
cough_idx = np.where(deriv > deriv_thresh)[0]
cough_t = t_win[cough_idx]
print(f"\nDetected cough/transient candidates: {len(cough_t)}")
for c in cough_t[:20]:
    print(f"  t={c:.1f}s ({c/60:.2f} min)")
if len(cough_t) > 20:
    print(f"  ... ({len(cough_t)-20} more)")

# Detect speech segments (voice-band RMS > threshold for ≥ 1s)
voice_thresh = np.percentile(body_voice_rms, 80)
is_voice = body_voice_rms > voice_thresh
# Run-length encode
runs_v = []
i = 0
while i < len(is_voice):
    if is_voice[i]:
        j = i
        while j < len(is_voice) and is_voice[j]: j += 1
        if (j - i) * (WIN/fs) >= 1.0:
            runs_v.append((t_win[i], t_win[j-1]))
        i = j
    else: i += 1
print(f"\nDetected voice/sustained-sound segments (≥1s): {len(runs_v)}")
for t0, t1 in runs_v[:10]:
    print(f"  t={t0:.1f}s - {t1:.1f}s ({t1-t0:.1f}s long)")
if len(runs_v) > 10:
    print(f"  ... ({len(runs_v)-10} more)")
