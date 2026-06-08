"""SensaPulse — 2-channel adaptive denoise prototype.

Input:  a SensaPulse session folder (audio.wav stereo: ch0=body, ch1=ambient)
Output: cleaned mono WAV + diagnostic spectrogram PNG + SNR report

Goal: prove that anh có thể tách audio môi trường ra khỏi heart/lung sounds
recorded ở body mic, sao cho bác sĩ nghe được như ống nghe truyền thống.

Three methods implemented (cheapest to most expensive in CPU):

  1. nlms      — Normalized LMS adaptive filter (time-domain, 1 frame look-ahead,
                 portable to CMSIS-DSP arm_lms_norm_f32 on Cortex-M4F).
  2. spectral  — Classical spectral subtraction (frame-by-frame STFT,
                 estimate ambient noise spectrum, subtract from body).
  3. wiener    — Wiener filter with cross-spectral noise reference
                 (best quality, batch-mode, not real-time-portable).

Caveat for v1.0 hardware: body mic MP23DB01HP has 130 Hz HPF, so heart S1/S2
fundamental (20-150 Hz) is already attenuated at the mic before any denoise.
Lung sounds (100-2500 Hz) are intact and serve as the primary validation
target for the algorithm. Heart sound bandwidth requires v2 mic upgrade
(IM73D122, 28 Hz HPF).

CLI:
    python -m tools.nlms_denoise SESSION_PATH [--mode nlms|spectral|wiener]
                                              [--filter-len 128]
                                              [--mu 0.05]
                                              [--out cleaned.wav]
                                              [--plot]

Example:
    python -m tools.nlms_denoise /Volumes/SENSASCOPE/SESSION_00004 --plot

Requirements: numpy, scipy (optional — only for spectrogram plot)

The output WAV is mono int16 16 kHz, listenable on any phone speaker /
headphones. Open in Audacity to compare against the raw body channel.
"""
from __future__ import annotations

import argparse
import json
import sys
import wave
from pathlib import Path
from typing import Tuple

import numpy as np


# ----------------------------- I/O helpers ---------------------------------

def read_session_audio(session_path: Path) -> Tuple[int, np.ndarray]:
    """Returns (sample_rate, ndarray[N, 2] int16). ch0=body, ch1=ambient."""
    audio_path = session_path / "audio.wav"
    if not audio_path.is_file():
        raise FileNotFoundError(f"audio.wav not found in {session_path}")
    with wave.open(str(audio_path), "rb") as w:
        sr = w.getframerate()
        nch = w.getnchannels()
        nframes = w.getnframes()
        raw = w.readframes(nframes)
    if w.getsampwidth() != 2:
        raise ValueError(f"expected 16-bit PCM, got width {w.getsampwidth()}")
    if nch != 2:
        raise ValueError(f"expected stereo (2 channels), got {nch}")
    arr = np.frombuffer(raw, dtype=np.int16).reshape(-1, nch)
    return sr, arr


def write_wav_mono(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    """Write a mono 16-bit PCM WAV."""
    s = np.asarray(samples)
    # Clip to int16 range and convert
    s_clipped = np.clip(s, -32768, 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(s_clipped.tobytes())


# ----------------------------- NLMS adaptive filter ------------------------

def nlms_denoise(body: np.ndarray, ambient: np.ndarray, mu: float = 0.05,
                 filter_len: int = 128, eps: float = 1e-6) -> np.ndarray:
    """Normalized LMS adaptive noise cancellation.

    Model:
        body[n]    = signal[n] + h * ambient[n]   (room leakage of ambient
                                                    into body mic via tissue
                                                    and air path)
        ambient[n] = noise reference (assumed signal-free)

    Adapt FIR filter w[k] to estimate h. Output cleaned signal:
        cleaned[n] = body[n] - sum(w * ambient[n-L+1..n])
        w_new = w_old + (mu / (||x||² + eps)) * x * cleaned[n]

    Returns cleaned signal (float32, full length). Convert to int16 before
    saving.

    Algorithm choice: NLMS over plain LMS because input power varies (snore
    bursts, breath spikes). NLMS divides by signal power → step size adapts.
    This is exactly what `arm_lms_norm_f32` does in CMSIS-DSP, so the
    firmware port is one-to-one.

    Typical tuning:
        filter_len = 64-256 taps (covers ~4-16 ms of room delay at 16 kHz)
        mu = 0.01-0.1 (higher = faster adapt but more residual noise)
    """
    body_f = body.astype(np.float32)
    amb_f  = ambient.astype(np.float32)

    n_samples = len(body_f)
    L = int(filter_len)
    w = np.zeros(L, dtype=np.float32)
    cleaned = np.zeros(n_samples, dtype=np.float32)

    # Pre-pad ambient with zeros so the filter has history for the first L-1 samples.
    amb_padded = np.concatenate([np.zeros(L - 1, dtype=np.float32), amb_f])

    for n in range(n_samples):
        x = amb_padded[n:n + L][::-1]   # most-recent first, like CMSIS-DSP
        y = float(np.dot(w, x))
        e = body_f[n] - y
        norm = float(np.dot(x, x)) + eps
        w = w + (mu / norm) * x * e
        cleaned[n] = e

    return cleaned


# ----------------------------- Spectral subtraction ------------------------

def spectral_subtract(body: np.ndarray, ambient: np.ndarray, sr: int,
                      frame_ms: float = 32.0, hop_ms: float = 8.0,
                      over_sub: float = 1.5, floor_db: float = -25.0) -> np.ndarray:
    """Berouti-style spectral subtraction with ambient mic as noise estimator.

    For each STFT frame:
        |Y(f)| = max( |Body(f)| - over_sub * |Ambient(f)|,
                      10^(floor_db/20) * |Body(f)| )
        phase = arg(Body(f))   (we never re-estimate phase — phase distortion
                                is tolerable for human listening at this rate)
    """
    body_f = body.astype(np.float32)
    amb_f  = ambient.astype(np.float32)

    n_frame = int(round(sr * frame_ms / 1000.0))
    n_hop   = int(round(sr * hop_ms   / 1000.0))
    win = np.hanning(n_frame).astype(np.float32)

    # Pad to integer multiple of hop length
    pad = (-len(body_f)) % n_hop
    if pad:
        body_f = np.concatenate([body_f, np.zeros(pad, dtype=np.float32)])
        amb_f  = np.concatenate([amb_f,  np.zeros(pad, dtype=np.float32)])

    n_frames = (len(body_f) - n_frame) // n_hop + 1
    out = np.zeros(len(body_f), dtype=np.float32)
    norm = np.zeros(len(body_f), dtype=np.float32)

    floor_lin = 10.0 ** (floor_db / 20.0)

    for i in range(n_frames):
        start = i * n_hop
        end   = start + n_frame
        b_frame = body_f[start:end] * win
        a_frame = amb_f[start:end] * win

        B = np.fft.rfft(b_frame)
        A = np.fft.rfft(a_frame)

        mag_b = np.abs(B)
        mag_a = np.abs(A)

        # Subtract scaled ambient magnitude, floor at floor_db * body
        mag_y = np.maximum(mag_b - over_sub * mag_a, floor_lin * mag_b)
        Y = mag_y * np.exp(1j * np.angle(B))

        y_frame = np.fft.irfft(Y, n=n_frame).astype(np.float32)
        out[start:end]  += y_frame * win
        norm[start:end] += win * win

    # Overlap-add normalization
    mask = norm > 1e-6
    out[mask] /= norm[mask]
    return out[:len(body)]


# ----------------------------- Wiener filter (cross-spectral) --------------

def wiener_denoise(body: np.ndarray, ambient: np.ndarray, sr: int,
                   frame_ms: float = 32.0, hop_ms: float = 8.0,
                   smooth: float = 0.85) -> np.ndarray:
    """Wiener filter with cross-spectral ambient reference.

    Per-frame:
        H(f) = max(0, 1 - Pn(f) / Py(f))
        Pn(f) = ambient power spectrum (with EMA smoothing across frames)
        Py(f) = body power spectrum (with EMA smoothing)
        out(f) = H(f) * Body(f)

    Smooth = 0.85 → ~6 frames of memory; balances responsiveness vs noise
    in the gain estimate.
    """
    body_f = body.astype(np.float32)
    amb_f  = ambient.astype(np.float32)

    n_frame = int(round(sr * frame_ms / 1000.0))
    n_hop   = int(round(sr * hop_ms   / 1000.0))
    win = np.hanning(n_frame).astype(np.float32)

    pad = (-len(body_f)) % n_hop
    if pad:
        body_f = np.concatenate([body_f, np.zeros(pad, dtype=np.float32)])
        amb_f  = np.concatenate([amb_f,  np.zeros(pad, dtype=np.float32)])

    n_frames = (len(body_f) - n_frame) // n_hop + 1
    out = np.zeros(len(body_f), dtype=np.float32)
    norm = np.zeros(len(body_f), dtype=np.float32)

    P_n_smooth = None
    P_y_smooth = None

    for i in range(n_frames):
        start = i * n_hop
        end   = start + n_frame
        b_frame = body_f[start:end] * win
        a_frame = amb_f[start:end] * win

        B = np.fft.rfft(b_frame)
        A = np.fft.rfft(a_frame)

        Py = np.abs(B) ** 2
        Pn = np.abs(A) ** 2

        if P_n_smooth is None:
            P_n_smooth = Pn
            P_y_smooth = Py
        else:
            P_n_smooth = smooth * P_n_smooth + (1 - smooth) * Pn
            P_y_smooth = smooth * P_y_smooth + (1 - smooth) * Py

        H = np.clip(1.0 - P_n_smooth / (P_y_smooth + 1e-8), 0.0, 1.0)
        Y = H * B

        y_frame = np.fft.irfft(Y, n=n_frame).astype(np.float32)
        out[start:end]  += y_frame * win
        norm[start:end] += win * win

    mask = norm > 1e-6
    out[mask] /= norm[mask]
    return out[:len(body)]


# ----------------------------- SNR estimate --------------------------------

def estimate_snr(cleaned: np.ndarray, ambient: np.ndarray) -> float:
    """Approximate SNR improvement: cleaned signal power vs ambient power, in dB.
    Not absolute SNR — just a comparative metric across modes.
    """
    p_cleaned = float(np.mean(cleaned.astype(np.float32) ** 2)) + 1e-12
    p_amb = float(np.mean(ambient.astype(np.float32) ** 2)) + 1e-12
    return 10.0 * np.log10(p_cleaned / p_amb)


# ----------------------------- Plot (optional) -----------------------------

def plot_diagnostic(body: np.ndarray, cleaned: np.ndarray, sr: int,
                    out_path: Path) -> None:
    """Spectrogram before/after side-by-side. Requires scipy + matplotlib.
    Skipped silently if those aren't installed.
    """
    try:
        import matplotlib.pyplot as plt
        from scipy.signal import spectrogram
    except ImportError:
        print("  (plot skipped: install matplotlib + scipy for spectrograms)")
        return

    fig, axes = plt.subplots(2, 1, figsize=(12, 6), sharex=True, sharey=True)
    for ax, sig, title in [(axes[0], body, "Body mic (raw)"),
                           (axes[1], cleaned, "Cleaned (denoised)")]:
        f, t, Sxx = spectrogram(sig.astype(np.float32), fs=sr,
                                nperseg=512, noverlap=384)
        ax.pcolormesh(t, f, 10 * np.log10(Sxx + 1e-12), shading="gouraud",
                      cmap="viridis", vmin=-100, vmax=-20)
        ax.set_ylabel("Hz")
        ax.set_title(title)
        ax.set_ylim(0, sr // 2)
    axes[-1].set_xlabel("seconds")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)
    print(f"  spectrogram → {out_path}")


# ----------------------------- CLI -----------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session", type=Path, help="Session folder (audio.wav inside)")
    ap.add_argument("--mode", choices=["nlms", "spectral", "wiener"],
                    default="nlms", help="Denoise algorithm (default nlms)")
    ap.add_argument("--filter-len", type=int, default=128,
                    help="NLMS FIR filter length in taps (default 128 = 8 ms @ 16 kHz)")
    ap.add_argument("--mu", type=float, default=0.05,
                    help="NLMS step size (default 0.05)")
    ap.add_argument("--out", type=Path, default=None,
                    help="Output WAV path (default: <session>/cleaned_<mode>.wav)")
    ap.add_argument("--plot", action="store_true",
                    help="Save spectrogram PNG next to output WAV")
    ap.add_argument("--limit-sec", type=float, default=None,
                    help="Process only first N seconds (faster iteration)")
    ap.add_argument("--fixed-channels", action="store_true",
                    help="audio.wav is already body-first (recorded by firmware "
                         "with the 2026-06-08 PDM L/R fix). Default assumes LEGACY "
                         "swapped channels (file col0=ambient, col1=body) and "
                         "un-swaps them — see app/src/audio.c + tools/load_session.py.")
    args = ap.parse_args()

    sr, stereo = read_session_audio(args.session)
    if args.limit_sec is not None:
        n = int(args.limit_sec * sr)
        stereo = stereo[:n]
    # Legacy firmware wrote the mics swapped (col0=ambient, col1=body).
    if args.fixed_channels:
        body, ambient = stereo[:, 0], stereo[:, 1]
    else:
        body, ambient = stereo[:, 1], stereo[:, 0]
    dur_s = len(body) / sr
    print(f"→ Loaded {dur_s:.1f} s of stereo audio @ {sr} Hz "
          f"({len(body):,} samples per channel)")
    print(f"  body    peak={int(np.max(np.abs(body))):>6}  "
          f"rms={float(np.sqrt(np.mean(body.astype(np.float32)**2))):.1f}")
    print(f"  ambient peak={int(np.max(np.abs(ambient))):>6}  "
          f"rms={float(np.sqrt(np.mean(ambient.astype(np.float32)**2))):.1f}")

    print(f"→ Running mode={args.mode}...")
    if args.mode == "nlms":
        cleaned = nlms_denoise(body, ambient,
                               mu=args.mu, filter_len=args.filter_len)
        print(f"  NLMS: filter_len={args.filter_len} taps, mu={args.mu}")
    elif args.mode == "spectral":
        cleaned = spectral_subtract(body, ambient, sr)
        print(f"  Spectral subtraction: over_sub=1.5, floor=-25 dB")
    elif args.mode == "wiener":
        cleaned = wiener_denoise(body, ambient, sr)
        print(f"  Wiener: smooth=0.85, 32 ms frame, 8 ms hop")

    # Metrics
    snr_body_in    = estimate_snr(body, ambient)
    snr_cleaned_in = estimate_snr(cleaned, ambient)
    snr_gain = snr_cleaned_in - snr_body_in
    p_body_in  = float(np.mean(body.astype(np.float32) ** 2))
    p_cleaned  = float(np.mean(cleaned ** 2))
    rms_atten_db = 10 * np.log10(p_body_in / (p_cleaned + 1e-12))

    print(f"\n=== Quality metrics ===")
    print(f"  Body-vs-ambient SNR (in)     : {snr_body_in:+6.2f} dB")
    print(f"  Cleaned-vs-ambient SNR (out) : {snr_cleaned_in:+6.2f} dB")
    print(f"  SNR gain                     : {snr_gain:+6.2f} dB")
    print(f"  RMS attenuation (body→cleaned): {rms_atten_db:+6.2f} dB")
    print(f"  (Negative attenuation = signal was preserved, positive = quieter output)")

    if args.out is None:
        args.out = args.session / f"cleaned_{args.mode}.wav"
    write_wav_mono(args.out, cleaned, sr)
    print(f"\n  cleaned WAV → {args.out}")

    if args.plot:
        plot_path = args.out.with_suffix(".png")
        plot_diagnostic(body, cleaned, sr, plot_path)

    print(f"\nDONE. Open both files in Audacity:")
    print(f"  raw body channel : {args.session / 'audio.wav'} (use ch0)")
    print(f"  cleaned output   : {args.out}")
    print(f"By ear, you should hear:")
    print(f"  - Same heart/lung sounds, similar volume")
    print(f"  - Less HVAC / room noise / TV / voice")
    print(f"  - May have some processing artifacts (especially with spectral/wiener)")
    print(f"\nIf NLMS doesn't denoise well, try:")
    print(f"  --mu 0.1 --filter-len 256   (more aggressive)")
    print(f"  --mode spectral             (different approach)")
    print(f"  --mode wiener               (best quality, slower)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
