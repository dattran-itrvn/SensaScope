"""v2 noise-cancellation — v1: tap+μ sweep, multi-stage pipeline.

Building on v0 finding that NLMS clearly beats spectral subtraction, this
script searches the (taps, μ) hyperparameter grid and adds:

 - Post-NLMS bandpass to 20-200 Hz (focus heart band)
 - Two orthogonal quality metrics beyond simple heart-band SNR:
     · kurtosis of band-passed envelope (peaky S1/S2 → high; flat noise → low)
     · autocorrelation peak strength at the heart-rate lag
 - WAV exports for best config
"""
# %% imports + load
from __future__ import annotations

import sys
import time
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import wave
from scipy import signal, stats

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00002")
OUT = REPO / "notebooks" / "figures"
OUT.mkdir(parents=True, exist_ok=True)

s = load_session(SESSION)
audio = s["audio"]
fs = s["fs_audio"]

# Use a shorter clean window (60 s) for sweep speed; once best params found
# we apply to the full 250 s window for final output.
T_LO, T_HI_SWEEP = 350, 410           # 60 s sweep window (mid-clean)
T_LO_FULL, T_HI_FULL = 300, 550       # 250 s full window

i_lo, i_hi = int(T_LO * fs), int(T_HI_SWEEP * fs)
body_sw = audio[i_lo:i_hi, 0].astype(np.float32) / 32768.0
amb_sw  = audio[i_lo:i_hi, 1].astype(np.float32) / 32768.0
print(f"sweep window: {T_LO}-{T_HI_SWEEP}s = {len(body_sw)/fs:.0f}s = {len(body_sw):,} samples")

# %% NLMS — vectorized inner loop is ~3-4× faster than pure Python
def nlms(ref: np.ndarray, tgt: np.ndarray, n_taps: int, mu: float) -> np.ndarray:
    N = len(tgt)
    w = np.zeros(n_taps, dtype=np.float32)
    cleaned = np.zeros(N, dtype=np.float32)
    eps = 1e-6
    for n in range(n_taps, N):
        x = ref[n - n_taps + 1 : n + 1][::-1]
        y_hat = w @ x
        e = tgt[n] - y_hat
        cleaned[n] = e
        w += (mu / ((x @ x) + eps)) * e * x
    return cleaned

# %% metrics
sos_hb = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")

def heart_snr_db(x):
    inband = signal.sosfiltfilt(sos_hb, x)
    out = x - inband
    return 10 * np.log10(np.mean(inband**2) / (np.mean(out**2) + 1e-12))

def envelope_kurtosis(x):
    """Heart sounds → spikes (S1/S2) → high kurtosis. Noise → low. >3 = super-Gaussian."""
    bp = signal.sosfiltfilt(sos_hb, x)
    env = np.abs(signal.hilbert(bp))
    return stats.kurtosis(env, fisher=False)   # 3 = Gaussian baseline

def hr_peak_strength(x):
    """Autocorr peak in 0.4-1.5 s range divided by stdev → SNR proxy for periodicity."""
    bp = signal.sosfiltfilt(sos_hb, x)
    env = np.abs(signal.hilbert(bp))
    env = env - env.mean()
    ac = signal.correlate(env, env, mode="full")
    ac = ac[len(ac) // 2 :]
    ac = ac / (ac[0] + 1e-12)               # normalize
    lo, hi = int(0.4 * fs), int(1.5 * fs)
    peak = ac[lo:hi].max()
    baseline_std = ac[hi : 2 * hi].std()
    return peak / (baseline_std + 1e-9), 60.0 / ((lo + np.argmax(ac[lo:hi])) / fs)

# %% baseline
snr0 = heart_snr_db(body_sw)
kurt0 = envelope_kurtosis(body_sw)
peak0, bpm0 = hr_peak_strength(body_sw)
print(f"baseline body: SNR={snr0:+.2f}dB  envKurt={kurt0:.2f}  "
      f"HRpeak={peak0:.2f}σ ({bpm0:.1f} BPM)")

# %% Hyperparameter sweep
print("\n=== NLMS hyperparameter sweep ===")
print(f"{'taps':>6} {'μ':>6}  {'SNR(dB)':>8}  {'kurt':>5}  {'HRpk(σ)':>7}  {'BPM':>5}  {'sec':>5}")
results = []
for taps in [64, 128, 256, 512]:
    for mu in [0.1, 0.3, 0.5]:
        t0 = time.time()
        clean = nlms(amb_sw, body_sw, taps, mu)
        elapsed = time.time() - t0
        snr = heart_snr_db(clean)
        kurt = envelope_kurtosis(clean)
        peak, bpm = hr_peak_strength(clean)
        results.append((taps, mu, snr, kurt, peak, bpm, elapsed))
        print(f"{taps:>6} {mu:>6.2f}  {snr:>+8.2f}  {kurt:>5.2f}  "
              f"{peak:>7.2f}  {bpm:>5.1f}  {elapsed:>5.1f}")

# %% Pick best by HR autocorr peak (more orthogonal than SNR alone)
best = max(results, key=lambda r: r[4])     # r[4] = HRpk
print(f"\nbest by HR-peak: taps={best[0]} μ={best[1]}  → "
      f"SNR={best[2]:+.2f}dB, kurt={best[3]:.2f}, HRpk={best[4]:.2f}σ, BPM={best[5]:.1f}")

# %% Apply best to full 250s window and save
print(f"\napplying best ({best[0]} taps, μ={best[1]}) to full {T_LO_FULL}-{T_HI_FULL}s window…")
i_lo2, i_hi2 = int(T_LO_FULL * fs), int(T_HI_FULL * fs)
body_full = audio[i_lo2:i_hi2, 0].astype(np.float32) / 32768.0
amb_full  = audio[i_lo2:i_hi2, 1].astype(np.float32) / 32768.0
t0 = time.time()
body_clean = nlms(amb_full, body_full, best[0], best[1])
print(f"  NLMS full window: {time.time()-t0:.1f}s")

# Post-stage: bandpass to heart range — final output stage
body_heart = signal.sosfiltfilt(sos_hb, body_clean)

# Metrics on full window
print(f"  raw body:        SNR={heart_snr_db(body_full):+.2f}dB  "
      f"kurt={envelope_kurtosis(body_full):.2f}  HRpk={hr_peak_strength(body_full)[0]:.2f}σ")
print(f"  NLMS body:       SNR={heart_snr_db(body_clean):+.2f}dB  "
      f"kurt={envelope_kurtosis(body_clean):.2f}  HRpk={hr_peak_strength(body_clean)[0]:.2f}σ")
print(f"  NLMS+heart-band: SNR={heart_snr_db(body_heart):+.2f}dB  "
      f"kurt={envelope_kurtosis(body_heart):.2f}  HRpk={hr_peak_strength(body_heart)[0]:.2f}σ")

# %% save WAVs for A/B listen
def save_wav(path, x):
    peak = np.abs(x).max() + 1e-9
    data = ((x / peak) * 0.95 * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(fs)
        w.writeframes(data.tobytes())

save_wav(OUT / "v1_body_clean_NLMS.wav",   body_clean)
save_wav(OUT / "v1_body_heart_filtered.wav", body_heart)
print(f"saved WAVs to {OUT}/")

# %% Figure — final comparison
fig, axs = plt.subplots(3, 1, figsize=(13, 7), sharex=True)
for ax, sig_arr, name, cmap in [
    (axs[0], body_full,  "body raw",              "viridis"),
    (axs[1], body_clean, f"body NLMS (taps={best[0]}, μ={best[1]})", "viridis"),
    (axs[2], body_heart, "body NLMS + bandpass 20-200 Hz", "viridis"),
]:
    fs_, ts_, Sxx_ = signal.spectrogram(sig_arr, fs=fs, nperseg=2048, noverlap=1024)
    mask = fs_ <= 800
    p = 10 * np.log10(np.maximum(Sxx_[mask], 1e-12))
    pcm = ax.pcolormesh(ts_, fs_[mask], p, shading="auto", cmap=cmap,
                         vmin=p.max() - 60, vmax=p.max())
    ax.set_ylabel("Hz")
    snr_v = heart_snr_db(sig_arr)
    pk, bpm_v = hr_peak_strength(sig_arr)
    ax.set_title(f"{name}  (heart-SNR={snr_v:+.2f}dB, HRpk={pk:.2f}σ → {bpm_v:.1f} BPM)")
    plt.colorbar(pcm, ax=ax, label="dB")
axs[-1].set_xlabel(f"time in {T_LO_FULL}-{T_HI_FULL}s window (s)")
plt.tight_layout()
plt.savefig(OUT / "v1_final_compare.png", dpi=110)
plt.close()
print(f"saved {OUT / 'v1_final_compare.png'}")

# %% Heart-band envelope over 5s zoom — visually check S1/S2 separation
def hb_env(x, n0, n1):
    seg = x[n0:n1]
    bp = signal.sosfiltfilt(sos_hb, seg)
    env = signal.savgol_filter(np.abs(signal.hilbert(bp)), 201, 3)
    return bp, env

zoom_t0 = 100   # offset into the 250s window
n0 = int(zoom_t0 * fs); n1 = n0 + 5 * fs
seg_t = np.arange(n1 - n0) / fs

fig, axs = plt.subplots(3, 1, figsize=(13, 7), sharex=True)
for ax, sig_arr, name in [
    (axs[0], body_full,  "body raw"),
    (axs[1], body_clean, "body NLMS"),
    (axs[2], body_heart, "body NLMS + 20-200 Hz"),
]:
    bp, env = hb_env(sig_arr, n0, n1)
    ax.plot(seg_t, bp,  lw=0.4, color="navy",    label="bandpass")
    ax.plot(seg_t, env, lw=1.5, color="crimson", label="envelope")
    ax.set_ylabel("amp"); ax.legend(loc="upper right")
    ax.set_title(f"{name} — 5 s zoom @ +{zoom_t0}s")
axs[-1].set_xlabel("time (s)")
plt.tight_layout()
plt.savefig(OUT / "v1_zoom_5s.png", dpi=110)
plt.close()
print(f"saved {OUT / 'v1_zoom_5s.png'}")

print("\n=== done ===")
