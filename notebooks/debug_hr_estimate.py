"""Visualize what the HR estimator sees for a single posture segment.

Plot:
  (a) 5-s zoom of bandpassed body audio
  (b) Envelope (Hilbert + lowpass)
  (c) Time-domain peaks detected
  (d) Envelope spectrum 0-3 Hz with markers at candidate fundamentals

Lets us decide visually whether the algorithm's sub-harmonic picks are
actually correct (= real bradycardia) or wrong (= S1/S2 confusion).
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
from tools.noise_cancel import pipeline

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00001")
OUT = REPO / "notebooks" / "figures"

s = load_session(SESSION)
audio = s["audio"]; fs = s["fs_audio"]
body = audio[:, 0].astype(np.float32) / 32768
amb  = audio[:, 1].astype(np.float32) / 32768

# Inspect 3 representative segments
SEGMENTS = [
    ("đứng (P1)",        10, 22),    # should give ~85-90 (sitting/standing)
    ("nằm thẳng (P5)",  185, 265),   # algorithm said 49 — bradycardia or wrong?
    ("đứng (P9)",       460, 525),   # should give ~75-85
]

fig, axs = plt.subplots(len(SEGMENTS), 3, figsize=(16, 3.5 * len(SEGMENTS)))
sos_hb = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")
sos_lp = signal.butter(4, 8, btype="low", fs=fs, output="sos")

for row, (label, t0, t1) in enumerate(SEGMENTS):
    n0, n1 = int(t0 * fs), int(t1 * fs)
    body_seg = body[n0:n1]
    amb_seg  = amb[n0:n1]
    cleaned = pipeline(body_seg, amb_seg, fs=fs, n_taps=512, mu=0.1)
    bp = signal.sosfiltfilt(sos_hb, cleaned)
    env = np.abs(signal.hilbert(bp))
    env_s = signal.sosfiltfilt(sos_lp, env)

    # (a) 5-s zoom of bandpass
    zoom_n = 5 * fs
    z0 = len(bp) // 2 - zoom_n // 2
    t_zoom = np.arange(zoom_n) / fs
    axs[row, 0].plot(t_zoom, bp[z0:z0 + zoom_n], lw=0.5, color="navy")
    axs[row, 0].plot(t_zoom, env_s[z0:z0 + zoom_n], lw=1.5, color="crimson")
    axs[row, 0].set_title(f"{label} — 5-s zoom (bandpass + envelope)")
    axs[row, 0].set_ylabel("amp")
    axs[row, 0].set_xlabel("time (s)")

    # (b) Peak detection on envelope (find_peaks)
    min_dist = int(0.4 * fs)        # max 150 BPM
    th = env_s.mean() + 0.4 * env_s.std()
    peaks, _ = signal.find_peaks(env_s, distance=min_dist, height=th)
    intervals = np.diff(peaks) / fs
    if len(intervals) > 0:
        median_int = np.median(intervals)
        bpm_from_peaks = 60.0 / median_int
        # bimodality check: short-long alternating = S1+S2 → real period = sum of pair
        sorted_int = np.sort(intervals)
        cv = np.std(intervals) / max(1e-9, np.mean(intervals))
    else:
        median_int = 0
        bpm_from_peaks = 0
        cv = 0
    axs[row, 1].plot(np.arange(len(env_s)) / fs, env_s, lw=0.5, color="steelblue")
    axs[row, 1].plot(peaks / fs, env_s[peaks], "v", color="red", markersize=4)
    axs[row, 1].set_title(f"envelope peaks: n={len(peaks)}, "
                          f"median ΔT={median_int*1000:.0f} ms → {bpm_from_peaks:.0f} BPM "
                          f"(CV={cv:.2f})")
    axs[row, 1].set_xlabel("time (s)")
    axs[row, 1].set_ylabel("envelope")

    # (c) Envelope spectrum 0-3 Hz
    env_s = env_s - env_s.mean()
    N = len(env_s)
    win = signal.windows.hann(N)
    spec = np.abs(np.fft.rfft(env_s * win))
    freqs = np.fft.rfftfreq(N, 1 / fs)
    mask = (freqs >= 0.4) & (freqs <= 3.0)
    axs[row, 2].plot(freqs[mask], spec[mask], color="purple")
    peak_idx = np.argmax(spec[mask])
    f_peak = freqs[mask][peak_idx]
    axs[row, 2].axvline(f_peak, color="red", ls="--",
                        label=f"peak: {f_peak:.2f} Hz = {f_peak*60:.0f} BPM")
    if bpm_from_peaks > 0:
        axs[row, 2].axvline(bpm_from_peaks / 60, color="green", ls=":",
                            label=f"peak-count: {bpm_from_peaks:.0f} BPM")
    axs[row, 2].set_xlabel("freq (Hz)")
    axs[row, 2].set_ylabel("|FFT|")
    axs[row, 2].set_title("envelope spectrum (0.4-3 Hz)")
    axs[row, 2].legend(loc="upper right", fontsize=8)

plt.tight_layout()
plt.savefig(OUT / "debug_hr_3segments.png", dpi=110)
plt.close()
print(f"saved {OUT / 'debug_hr_3segments.png'}")
