"""Zoom on the structured protocol region — last 100s of SESSION_00002."""
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
duration = len(body) / fs

# Last 100 seconds
T0 = max(0, duration - 100)
T1 = duration
n0, n1 = int(T0 * fs), int(T1 * fs)
body_z = body[n0:n1]
amb_z  = amb[n0:n1]
t = np.arange(len(body_z)) / fs + T0

sos_lung = signal.butter(4, [60, 2000], btype="band", fs=fs, output="sos")
sos_voice = signal.butter(4, [300, 3000], btype="band", fs=fs, output="sos")
sos_heart = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")

lung_bp  = signal.sosfiltfilt(sos_lung, body_z)
voice_bp = signal.sosfiltfilt(sos_voice, body_z)
heart_bp = signal.sosfiltfilt(sos_heart, body_z)

# RMS in 100 ms windows
W = int(0.1 * fs)
def rms(x):
    n = (len(x) // W) * W
    return np.sqrt((x[:n] ** 2).reshape(-1, W).mean(axis=1))
t_w = np.arange(len(lung_bp) // W) * 0.1 + T0

fig, axs = plt.subplots(5, 1, figsize=(15, 11), sharex=True)
axs[0].plot(t_w, rms(lung_bp),  color="steelblue", lw=0.8)
axs[0].set_ylabel("lung 60-2k Hz RMS"); axs[0].set_title("body — lung band")
axs[1].plot(t_w, rms(voice_bp), color="darkorange", lw=0.8, label="body")
axs[1].plot(t_w, rms(signal.sosfiltfilt(sos_voice, amb_z)),
             color="purple", alpha=0.6, lw=0.8, label="ambient")
axs[1].set_ylabel("voice 300-3k Hz RMS"); axs[1].legend()
axs[1].set_title("voice band — speech detector")
axs[2].plot(t_w, rms(heart_bp), color="crimson", lw=0.8)
axs[2].set_ylabel("heart 20-200 Hz RMS"); axs[2].set_title("body — heart band (should be continuous)")

# Spectrogram 0-3kHz
f_, ts_, S_ = signal.spectrogram(body_z, fs=fs, nperseg=2048, noverlap=1024)
mask = f_ <= 3000
p = 10 * np.log10(np.maximum(S_[mask], 1e-12))
pcm = axs[3].pcolormesh(ts_ + T0, f_[mask], p, shading="auto", cmap="viridis",
                          vmin=p.max()-60, vmax=p.max())
axs[3].set_ylabel("freq (Hz)")
axs[3].set_title("spectrogram (0-3 kHz)")
plt.colorbar(pcm, ax=axs[3], label="dB")

# IMU
imu_t = (imu.t_us.values - imu.t_us.iloc[0]) / 1e6
sel = (imu_t >= T0) & (imu_t <= T1)
accel = np.sqrt(imu.ax**2 + imu.ay**2 + imu.az**2)[sel] / 16384.0
axs[4].plot(imu_t[sel], accel, color="green", lw=0.5)
axs[4].axhline(1.0, color="grey", ls=":", lw=0.4)
axs[4].set_ylabel("|accel| (g)")
axs[4].set_xlabel("time (s) from session start")
axs[4].set_title("IMU motion — large excursions = body movement / deep breath")
axs[4].set_xlim(T0, T1)

# Add phase markers for the typical 6-phase protocol if last 60 sec is the protocol
# (we'll annotate after looking)
for ax in axs[:4]:
    ax.set_xlim(T0, T1)

plt.tight_layout()
plt.savefig(OUT / "lung_zoom_last100s.png", dpi=110)
plt.close()
print(f"saved {OUT / 'lung_zoom_last100s.png'}")
