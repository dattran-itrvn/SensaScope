"""v2 noise-cancellation algorithm — first pass.

Two baseline methods on the clean window (300-550 s) of SESSION_00002:
  A. Spectral subtraction: |Y_clean|² = |Y_body|² - α·|Y_amb|² with spectral floor.
  B. Normalized-LMS adaptive filter: predict ambient leakage into body, subtract.

Outputs:
  - Spectrograms: body raw vs ambient vs cleaned (both methods)
  - SNR-in-heart-band metric (20-200 Hz vs everywhere)
  - Cleaned WAV files for A/B listen
"""
# %% imports + load
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import wave
from scipy import signal

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00002")
OUT = REPO / "notebooks" / "figures"
OUT.mkdir(parents=True, exist_ok=True)

s = load_session(SESSION)
audio = s["audio"]
fs    = s["fs_audio"]

# Clean window — IMU stdev showed minimal motion from ~300s onward
T_LO, T_HI = 300, 550
i_lo, i_hi = int(T_LO * fs), int(T_HI * fs)
body_raw = audio[i_lo:i_hi, 0].astype(np.float32) / 32768.0
amb_raw  = audio[i_lo:i_hi, 1].astype(np.float32) / 32768.0
N = len(body_raw)
print(f"window: {T_LO}-{T_HI} s = {N/fs:.1f} s = {N:,} samples")
print(f"body RMS: {np.sqrt(np.mean(body_raw**2)):.4f}, ambient RMS: {np.sqrt(np.mean(amb_raw**2)):.4f}")

# %% metric helpers
def heartband_snr_db(x: np.ndarray, fs: int = 16000) -> float:
    """SNR proxy: energy in 20-200 Hz (heart band) vs energy outside.
    Higher = more concentrated in heart band = better signal quality
    for a stethoscope recording."""
    sos = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")
    inband = signal.sosfiltfilt(sos, x)
    outband = x - inband
    P_in = np.mean(inband**2)
    P_out = np.mean(outband**2) + 1e-12
    return 10.0 * np.log10(P_in / P_out)

def save_wav_mono(path: Path, x: np.ndarray, fs: int = 16000) -> None:
    """Save mono int16 WAV. Auto-normalize to ±0.95 to avoid clipping."""
    peak = np.abs(x).max() + 1e-9
    norm = (x / peak) * 0.95
    data = (norm * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(fs)
        w.writeframes(data.tobytes())

# %% baseline metric
snr_body_raw = heartband_snr_db(body_raw, fs)
snr_amb_raw  = heartband_snr_db(amb_raw, fs)
print(f"baseline heart-band SNR: body={snr_body_raw:+.2f} dB, ambient={snr_amb_raw:+.2f} dB")

# %% method A — spectral subtraction
NPERSEG = 2048
NOVERLAP = 1536

f, t, Yb = signal.stft(body_raw, fs=fs, nperseg=NPERSEG, noverlap=NOVERLAP)
_, _, Ya = signal.stft(amb_raw,  fs=fs, nperseg=NPERSEG, noverlap=NOVERLAP)
P_b = np.abs(Yb) ** 2
P_a = np.abs(Ya) ** 2

# alpha = over-subtraction factor (1=exact, 1.5-3 = aggressive)
# beta  = spectral floor (prevents musical noise from over-subtraction)
ALPHA = 2.0
BETA = 0.05
P_clean = np.maximum(P_b - ALPHA * P_a, BETA * P_b)
Y_clean_A = np.sqrt(P_clean) * np.exp(1j * np.angle(Yb))
_, body_specsub = signal.istft(Y_clean_A, fs=fs, nperseg=NPERSEG, noverlap=NOVERLAP)
body_specsub = body_specsub[:N]

snr_specsub = heartband_snr_db(body_specsub, fs)
print(f"spectral subtraction (α={ALPHA}, β={BETA}): SNR={snr_specsub:+.2f} dB "
      f"(Δ={snr_specsub-snr_body_raw:+.2f} dB)")

# %% method B — normalized LMS adaptive filter
# Predict ambient leakage into body using last N_TAPS ambient samples; subtract.
# NLMS is robust to varying signal levels. Use scipy filter-style loop, ~2s for 250s data.
def nlms_filter(x_ref: np.ndarray, x_target: np.ndarray,
                n_taps: int = 128, mu: float = 0.1) -> tuple[np.ndarray, np.ndarray]:
    """NLMS: cleaned[n] = target[n] - sum(w * ref[n-taps+1..n])."""
    N = len(x_target)
    w = np.zeros(n_taps, dtype=np.float32)
    cleaned = np.zeros(N, dtype=np.float32)
    eps = 1e-6
    for n in range(n_taps, N):
        x = x_ref[n - n_taps + 1 : n + 1][::-1]    # most recent first
        y_hat = w @ x
        e = x_target[n] - y_hat
        cleaned[n] = e
        norm = (x @ x) + eps
        w = w + (mu / norm) * e * x
    return cleaned, w

print("running NLMS (this takes ~10-20 s on 250s window)...")
body_nlms, w_nlms = nlms_filter(amb_raw, body_raw, n_taps=128, mu=0.3)
snr_nlms = heartband_snr_db(body_nlms, fs)
print(f"NLMS (taps=128, μ=0.3): SNR={snr_nlms:+.2f} dB "
      f"(Δ={snr_nlms-snr_body_raw:+.2f} dB)")
print(f"NLMS filter weights peak: {np.abs(w_nlms).max():.4f}, "
      f"non-zero range ~taps 0-{np.argmax(np.abs(w_nlms))}")

# %% Figure A — before/after spectrograms (focus 0-1500 Hz)
fig, axs = plt.subplots(4, 1, figsize=(13, 9), sharex=True)

for ax, sig_arr, name, cmap in [
    (axs[0], body_raw,      "body raw",            "viridis"),
    (axs[1], amb_raw,       "ambient raw",         "inferno"),
    (axs[2], body_specsub,  "body after spec-sub", "viridis"),
    (axs[3], body_nlms,     "body after NLMS",     "viridis"),
]:
    fs_, ts_, Sxx_ = signal.spectrogram(sig_arr, fs=fs, nperseg=2048, noverlap=1024)
    mask = fs_ <= 1500
    p = 10 * np.log10(np.maximum(Sxx_[mask], 1e-12))
    pcm = ax.pcolormesh(ts_, fs_[mask], p, shading="auto", cmap=cmap,
                         vmin=p.max() - 60, vmax=p.max())
    ax.set_ylabel("Hz")
    ax.set_title(f"{name}  (heart-band SNR = "
                  f"{heartband_snr_db(sig_arr, fs):+.2f} dB)")
    plt.colorbar(pcm, ax=ax, label="dB")
axs[-1].set_xlabel(f"time within {T_LO}-{T_HI}s window (s)")
plt.tight_layout()
plt.savefig(OUT / "v2_A_spectrograms.png", dpi=110)
plt.close()
print(f"saved {OUT / 'v2_A_spectrograms.png'}")

# %% Figure B — 3-second zoom waveform + envelope compare
zoom_t0 = 100  # 100s into the 300-550 window = 400s into session
zoom_n0 = int(zoom_t0 * fs)
zoom_n1 = zoom_n0 + 3 * fs
seg_t = np.arange(zoom_n1 - zoom_n0) / fs

sos_hb = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")
def hb_env(x):
    bp = signal.sosfiltfilt(sos_hb, x[zoom_n0:zoom_n1])
    return bp, signal.savgol_filter(np.abs(signal.hilbert(bp)), 401, 3)

bp_raw, env_raw       = hb_env(body_raw)
bp_specsub, env_specsub = hb_env(body_specsub)
bp_nlms, env_nlms     = hb_env(body_nlms)

fig, axs = plt.subplots(3, 1, figsize=(13, 7), sharex=True)
for ax, bp, env, name in [
    (axs[0], bp_raw,     env_raw,     "body raw"),
    (axs[1], bp_specsub, env_specsub, "body spec-sub"),
    (axs[2], bp_nlms,    env_nlms,    "body NLMS"),
]:
    ax.plot(seg_t, bp, lw=0.5, color="navy", label="band-pass 20-200 Hz")
    ax.plot(seg_t, env, lw=1.5, color="crimson", label="envelope")
    ax.set_ylabel("amp")
    ax.set_title(f"{name} — 3-s zoom @ {T_LO+zoom_t0}s into session")
    ax.legend(loc="upper right")
axs[-1].set_xlabel("time (s)")
plt.tight_layout()
plt.savefig(OUT / "v2_B_zoom_compare.png", dpi=110)
plt.close()
print(f"saved {OUT / 'v2_B_zoom_compare.png'}")

# %% save WAVs for A/B listen
save_wav_mono(OUT / "body_raw_300-550.wav",    body_raw)
save_wav_mono(OUT / "ambient_raw_300-550.wav", amb_raw)
save_wav_mono(OUT / "body_specsub_300-550.wav", body_specsub)
save_wav_mono(OUT / "body_nlms_300-550.wav",   body_nlms)
print(f"saved 4 WAVs to {OUT}/ for A/B listen")

# %% summary
print()
print("=== heart-band SNR summary (higher = more signal concentrated in heart band) ===")
print(f"  body raw:           {snr_body_raw:+.2f} dB")
print(f"  ambient raw:        {snr_amb_raw:+.2f} dB")
print(f"  body + spec-sub:    {snr_specsub:+.2f} dB  (Δ={snr_specsub-snr_body_raw:+.2f})")
print(f"  body + NLMS:        {snr_nlms:+.2f} dB  (Δ={snr_nlms-snr_body_raw:+.2f})")
