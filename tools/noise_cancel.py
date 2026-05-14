"""NLMS-based noise cancellation for stethoscope audio.

Pipeline: body_mic - NLMS(ambient_mic) → bandpass(20-200 Hz) → cleaned heart.

Empirically tuned on bench session 2 (2026-05-14): 512 taps, μ=0.1 gives
+24.6 dB SNR improvement in the heart band over raw, with autocorrelation
periodicity strength jumping from 3.95σ to 5.4σ.

Computational fit for on-device port (Cortex-M4 + FPU):
  - NLMS per sample: 512 MAC + 512 update = 1024 FLOPS
  - Per second @ 16 kHz: 16e3 × 1024 = 16.4 MFLOPS
  - nRF52840 M4 @ 64 MHz peak ~64 MFLOPS → ~25 % budget. Feasible.

This module is intentionally framework-free (only numpy + scipy.signal)
so it can run as a PC-side analysis tool now and as a reference for
future on-device fixed-point port.
"""
from __future__ import annotations

import numpy as np
from scipy import signal


def nlms_subtract(reference: np.ndarray, target: np.ndarray,
                  n_taps: int = 512, mu: float = 0.1) -> np.ndarray:
    """Adaptive subtraction: `target - h * reference` with h learned online.

    Models the ambient → body acoustic leakage as a short FIR filter and
    removes it. Returns the residual (cleaned target).

    `reference` and `target` must be 1-D float arrays of equal length.
    """
    assert reference.shape == target.shape, "reference and target shape mismatch"
    ref = reference.astype(np.float32, copy=False)
    tgt = target.astype(np.float32, copy=False)
    N = len(tgt)
    w = np.zeros(n_taps, dtype=np.float32)
    cleaned = np.zeros(N, dtype=np.float32)
    eps = 1e-6
    for n in range(n_taps, N):
        x = ref[n - n_taps + 1 : n + 1][::-1]
        y_hat = w @ x
        e = tgt[n] - y_hat
        cleaned[n] = e
        norm = (x @ x) + eps
        w += (mu / norm) * e * x
    return cleaned


def heart_filter(x: np.ndarray, fs: int = 16000,
                 band_lo: float = 20.0, band_hi: float = 200.0) -> np.ndarray:
    """Bandpass to the heart-sound band (default 20-200 Hz)."""
    sos = signal.butter(4, [band_lo, band_hi], btype="band", fs=fs, output="sos")
    return signal.sosfiltfilt(sos, x)


def pipeline(body: np.ndarray, ambient: np.ndarray, fs: int = 16000,
             n_taps: int = 512, mu: float = 0.1,
             band_lo: float = 20.0, band_hi: float = 200.0) -> np.ndarray:
    """End-to-end: NLMS-subtract ambient leakage, then bandpass to heart band."""
    cleaned = nlms_subtract(ambient, body, n_taps=n_taps, mu=mu)
    return heart_filter(cleaned, fs=fs, band_lo=band_lo, band_hi=band_hi)


# ---------- metrics for evaluating cleaning quality --------------------------

def heart_snr_db(x: np.ndarray, fs: int = 16000,
                 band_lo: float = 20.0, band_hi: float = 200.0) -> float:
    """Energy ratio between the heart band and everything else.
    Higher = signal more concentrated in heart band."""
    sos = signal.butter(4, [band_lo, band_hi], btype="band", fs=fs, output="sos")
    inband = signal.sosfiltfilt(sos, x)
    out = x - inband
    return 10.0 * np.log10(np.mean(inband ** 2) / (np.mean(out ** 2) + 1e-12))


def hr_periodicity(x: np.ndarray, fs: int = 16000,
                   band_lo: float = 20.0, band_hi: float = 200.0,
                   hr_min_bpm: float = 40, hr_max_bpm: float = 150,
                   ) -> tuple[float, float]:
    """Autocorrelation peak strength + estimated BPM.

    Returns (peak_strength_sigma, bpm). Peak ≥ 3σ = clear periodicity.
    """
    sos = signal.butter(4, [band_lo, band_hi], btype="band", fs=fs, output="sos")
    bp = signal.sosfiltfilt(sos, x)
    env = np.abs(signal.hilbert(bp))
    env = env - env.mean()
    ac = signal.correlate(env, env, mode="full")
    ac = ac[len(ac) // 2 :]
    ac = ac / (ac[0] + 1e-12)
    lo = int(60.0 / hr_max_bpm * fs)
    hi = int(60.0 / hr_min_bpm * fs)
    peak_lag = lo + int(np.argmax(ac[lo:hi]))
    peak_val = ac[peak_lag]
    baseline = ac[hi : 2 * hi].std()
    return peak_val / (baseline + 1e-9), 60.0 / (peak_lag / fs)
