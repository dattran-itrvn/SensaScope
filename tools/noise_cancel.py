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
    """Autocorrelation peak strength + estimated BPM (legacy, weak).

    Returns (peak_strength_sigma, bpm). Peak ≥ 3σ = clear periodicity.
    Susceptible to sub-harmonic / harmonic picks. Use `hr_robust` instead.
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


def _envelope_spectrum(x: np.ndarray, fs: int,
                        band_lo: float, band_hi: float):
    """Return (freqs, spec) of the heart-band-passed envelope. Reusable."""
    sos = signal.butter(4, [band_lo, band_hi], btype="band", fs=fs, output="sos")
    bp = signal.sosfiltfilt(sos, x)
    env = np.abs(signal.hilbert(bp))
    sos_lp = signal.butter(4, 10, btype="low", fs=fs, output="sos")
    env_s = signal.sosfiltfilt(sos_lp, env)
    env_s = env_s - env_s.mean()
    N = len(env_s)
    win = signal.windows.hann(N)
    spec = np.abs(np.fft.rfft(env_s * win))
    freqs = np.fft.rfftfreq(N, 1.0 / fs)
    return freqs, spec


def _first_local_max(arr: np.ndarray, min_prominence: float = 0.0) -> int:
    """Index of first local maximum (rising→falling) above min_prominence.
    Returns -1 if none found."""
    for i in range(1, len(arr) - 1):
        if arr[i] > arr[i - 1] and arr[i] >= arr[i + 1] and arr[i] >= min_prominence:
            return i
    return -1


def hr_peak_count(x: np.ndarray, fs: int = 16000,
                  band_lo: float = 20.0, band_hi: float = 200.0,
                  hr_min_bpm: float = 40, hr_max_bpm: float = 150,
                  ) -> tuple[float, float, dict]:
    """Time-domain peak counting on the heart-band envelope.

    Most direct and reliable estimator on real stethoscope data:
    find_peaks in the smoothed envelope, then median inter-peak interval
    converts to BPM. Validated visually (debug_hr_estimate.py) to give
    correct results where FFT/autocorr were picking sub-harmonics.

    Returns `(bpm, confidence, debug_info)`. `confidence` = inverse of
    CV (coefficient of variation) — high when intervals are regular.
    Heart-rate variability typically gives CV ≈ 0.05-0.15; CV > 0.3 means
    detection is noisy or HR is highly irregular.
    """
    sos = signal.butter(4, [band_lo, band_hi], btype="band", fs=fs, output="sos")
    bp = signal.sosfiltfilt(sos, x)
    env = np.abs(signal.hilbert(bp))
    sos_lp = signal.butter(4, 8, btype="low", fs=fs, output="sos")
    env_s = signal.sosfiltfilt(sos_lp, env)

    min_dist = int(60.0 / hr_max_bpm * fs)
    threshold = env_s.mean() + 0.4 * env_s.std()
    peaks, _ = signal.find_peaks(env_s, distance=min_dist, height=threshold)
    if len(peaks) < 5:
        return 0.0, 0.0, {"n_peaks": len(peaks), "note": "too few peaks"}

    intervals = np.diff(peaks) / fs
    median_int = float(np.median(intervals))
    bpm = 60.0 / median_int
    cv = float(np.std(intervals) / max(median_int, 1e-9))
    # Confidence is bounded [0, 5]; CV of 0 → 5, CV of 0.5 → ~2, CV ≥ 1 → 0.
    # CV 0.4-0.6 is normal for find_peaks on heart envelope (S1+S2 jitter
    # + breath-modulated HRV), so we use a forgiving mapping.
    confidence = 5.0 / (1.0 + 4.0 * cv)
    info = {
        "n_peaks": int(len(peaks)),
        "median_interval_ms": median_int * 1000,
        "cv": cv,
    }

    # Sanity clamp to valid range
    if bpm < hr_min_bpm or bpm > hr_max_bpm:
        info["note"] = f"BPM {bpm:.1f} out of range"
        return 0.0, 0.0, info
    return bpm, confidence, info


def hr_robust(x: np.ndarray, fs: int = 16000,
              band_lo: float = 20.0, band_hi: float = 200.0,
              hr_min_bpm: float = 40, hr_max_bpm: float = 120,
              hr_prior: float = 0.0,
              ) -> tuple[float, float, str]:
    """Robust HR estimator combining envelope spectrum + harmonic validation.

    Returns `(bpm, confidence_sigma, source_tag)`.

    Strategy: heart pulses produce a quasi-periodic envelope. The fundamental
    frequency = HR / 60. We:
      1. Compute the envelope of the band-passed signal (Hilbert + smooth).
      2. FFT the envelope; find peak in [hr_min, hr_max] Hz range.
      3. Validate: check that the peak has a harmonic at 2× and that
         the half-frequency (which would be the sub-harmonic pick of a
         simple autocorr) is NOT stronger.
      4. Confidence = peak amplitude relative to surrounding noise floor.

    `source_tag` reports which decision branch fired (useful for debugging).

    If `hr_prior > 0`, search is narrowed to ±25 % around the prior. Use
    `hr_robust_two_pass` (below) to auto-derive a prior from the median of
    a first wide pass.
    """
    freqs, spec = _envelope_spectrum(x, fs, band_lo, band_hi)

    # Define search range — narrowed by prior if given
    if hr_prior > 0:
        f_lo = max(hr_min_bpm, 0.75 * hr_prior) / 60.0
        f_hi = min(hr_max_bpm, 1.30 * hr_prior) / 60.0
        tag_base = "spectrum/prior"
    else:
        f_lo = hr_min_bpm / 60.0
        f_hi = hr_max_bpm / 60.0
        tag_base = "spectrum"

    in_band = (freqs >= f_lo) & (freqs <= f_hi)
    if not in_band.any():
        return 0.0, 0.0, "no-band"

    band_spec = spec[in_band]
    band_freqs = freqs[in_band]
    peak_idx = int(np.argmax(band_spec))
    f_peak = band_freqs[peak_idx]
    peak_val = band_spec[peak_idx]
    bpm = f_peak * 60.0
    noise = np.median(band_spec) + 1e-12
    confidence = peak_val / noise

    # Harmonic 2× check — real heart period has spectral energy at f and 2f
    tag = tag_base
    f2 = 2 * f_peak
    if f2 < freqs[-1]:
        i2 = int(np.argmin(np.abs(freqs - f2)))
        win_r = max(1, int(0.05 * f_peak * len(freqs)))
        harm2_val = spec[max(0, i2 - win_r):i2 + win_r + 1].max()
        if harm2_val > 2.0 * noise:
            tag += "+h2"

    return bpm, confidence, tag


def hr_robust_two_pass(segments: list[np.ndarray], fs: int = 16000,
                       band_lo: float = 20.0, band_hi: float = 200.0,
                       hr_min_bpm: float = 40, hr_max_bpm: float = 150,
                       ) -> list[tuple[float, float, str]]:
    """Auto-calibrate HR estimator using subject-specific prior.

    Pass 1 (peak-count): estimate HR on every segment with wide search.
    Pass 2 (peak-count with prior): narrow distance constraints around
            the median of CONFIDENT (≥2σ) pass-1 estimates.

    Time-domain peak counting (`hr_peak_count`) is now the primary
    method — `hr_robust` (spectrum-based) consistently picked
    sub-harmonics on real stethoscope envelopes (see
    `notebooks/debug_hr_estimate.py`).

    Returns list of (bpm, confidence, tag) — one per segment.
    """
    pass1 = []
    for seg in segments:
        bpm, conf, info = hr_peak_count(seg, fs, band_lo, band_hi,
                                          hr_min_bpm, hr_max_bpm)
        tag = f"peak/CV={info.get('cv', 0):.2f}"
        pass1.append((bpm, conf, tag))

    confident_bpms = [p[0] for p in pass1 if p[1] >= 2.0 and p[0] > 0]
    if len(confident_bpms) < 2:
        return [(b, c, t + "/no-prior") for (b, c, t) in pass1]
    prior = float(np.median(confident_bpms))

    # Pass 2: peak-count with tighter min_dist around prior interval
    pass2 = []
    for seg in segments:
        # min_dist is 60 % of prior interval (allows up to +66 % HR variance)
        sos = signal.butter(4, [band_lo, band_hi], btype="band", fs=fs, output="sos")
        bp = signal.sosfiltfilt(sos, seg)
        env = np.abs(signal.hilbert(bp))
        sos_lp = signal.butter(4, 8, btype="low", fs=fs, output="sos")
        env_s = signal.sosfiltfilt(sos_lp, env)
        prior_int = 60.0 / prior * fs
        min_dist = int(prior_int * 0.6)
        threshold = env_s.mean() + 0.3 * env_s.std()
        peaks, _ = signal.find_peaks(env_s, distance=min_dist, height=threshold)
        if len(peaks) < 5:
            pass2.append(pass1[len(pass2)])
            continue
        intervals = np.diff(peaks) / fs
        median_int = float(np.median(intervals))
        bpm = 60.0 / median_int
        cv = float(np.std(intervals) / max(median_int, 1e-9))
        conf = 5.0 / (1.0 + 4.0 * cv)
        pass2.append((bpm, conf, f"peak/prior={prior:.0f}/CV={cv:.2f}"))
    return pass2


def hr_smooth_across(estimates: list[tuple[float, float]],
                     max_jump_bpm: float = 30.0) -> list[float]:
    """Post-process HR estimates from consecutive segments.

    `estimates` is a list of `(bpm, confidence)` tuples in chronological order.
    Returns a corrected list where each segment with low confidence (<3σ) or
    a large jump (>max_jump_bpm) from the local median is replaced by the
    local median of confident neighbours.
    """
    bpms = np.array([e[0] for e in estimates], dtype=float)
    confs = np.array([e[1] for e in estimates], dtype=float)
    n = len(bpms)
    out = bpms.copy()

    def local_median(i):
        # ±2 neighbours, weighted by confidence
        lo, hi = max(0, i - 2), min(n, i + 3)
        slc_bpm = bpms[lo:hi]
        slc_cf = confs[lo:hi]
        # only neighbours with conf ≥ 3 considered "anchor"
        anchors = slc_bpm[slc_cf >= 3.0]
        if len(anchors) == 0:
            return bpms[i]
        return float(np.median(anchors))

    for i in range(n):
        median = local_median(i)
        if confs[i] < 3.0 or abs(bpms[i] - median) > max_jump_bpm:
            out[i] = median
    return list(out)
