"""SensaPulse — honest ANC evaluation metrics (TASKS.md #37).

WHY THIS EXISTS
---------------
The legacy quality numbers — `heart_snr_db` (tools/noise_cancel.py) and
`heartband_snr_db` (notebooks/v2_noise_cancel_v0.py) — are TAUTOLOGICAL:
they measure the energy ratio between the heart band and everything else
*after* the pipeline already band-passed to the heart band. Any filter that
removes out-of-band energy makes them go up, even one that destroys the
cardiac signal. They cannot answer the question that matters for #37:

    Does ambient-mic ANC remove ENVIRONMENT from the body channel
    WITHOUT also removing the heart/lung signal?

This module answers it with two non-circular measurements:

1. INTER-MIC COHERENCE BY FREQUENCY (`coherence_by_band`)
   Welch magnitude-squared coherence Cxy(f) between the body and ambient
   channels. If the ambient mic is coherent with the body mic *inside the
   heart/lung bands*, then the ambient channel carries the very signal we
   want to keep — and any adaptive canceller using ambient as its reference
   will subtract that signal out. High in-band coherence ⇒ ANC is dangerous
   here, regardless of what an SNR number says.

2. CARDIAC CYCLE FIDELITY, BEFORE vs AFTER NLMS (`cycle_fidelity`)
   - autocorr_sigma: strength (in σ over the local baseline) of the
     autocorrelation peak of the heart-band envelope — how periodic the
     signal is. (reuses noise_cancel.hr_periodicity)
   - hr_cv: coefficient of variation of inter-beat intervals from the
     time-domain peak detector. (reuses noise_cancel.hr_peak_count)
   A canceller that helps preserves or sharpens periodicity (σ up / steady,
   CV steady or down). A canceller that *destroys* the signal can still
   raise heart_snr_db while autocorr_sigma collapses and CV blows up — that
   is the failure this catches.

VERDICT (`evaluate_anc`) is derived ONLY from coherence + fidelity, never
from heart_snr_db (which is printed alongside, clearly labelled, for context
with the historical numbers only).

Framework-free: numpy + scipy.signal, reusing tools/noise_cancel.py helpers.

CLI:
    python -m tools.honest_metrics SESSION_PATH
        [--start-sec S] [--end-sec E]   # analysis window (pick a quiet
                                        #   heart-only stretch for fidelity)
        [--n-taps 512] [--mu 0.1]       # NLMS params (match noise_cancel)
        [--channels fixed|legacy]       # default fixed (post-2026-06-08 FW)
"""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Optional

import numpy as np
from scipy import signal

from tools.load_session import _read_wav
from tools import noise_cancel as nc


# Frequency bands for coherence reporting. Heart fundamental is attenuated by
# the body mic's 130 Hz HPF (MP23DB01HP) so the heart band realistically
# survives mostly 130-150 Hz; lung sounds (100-2500 Hz) are the primary
# validation target; voice/ambient energy dominates 300-3000 Hz.
_BANDS = {
    "heart_20_150":      (20.0, 150.0),
    "lung_150_1000":     (150.0, 1000.0),
    "voice_300_3000":    (300.0, 3000.0),
    "broadband_20_4000": (20.0, 4000.0),
}


def coherence_by_band(body: np.ndarray, ambient: np.ndarray, fs: int = 16000,
                      nperseg: int = 8192) -> dict:
    """Mean magnitude-squared coherence between body & ambient, per band.

    Returns {band_name: {"mean": float, "max": float, "f_at_max": float}}.
    Coherence is in [0, 1]: ~0 = the two mics share no linear structure in
    that band; →1 = the ambient channel linearly predicts the body channel
    there (so ANC referenced on ambient would cancel that content).

    nperseg=8192 @ 16 kHz → ~1.95 Hz resolution, enough to resolve the heart
    band while still averaging many segments on a ≥100 s recording.
    """
    b = np.asarray(body, dtype=np.float64)
    a = np.asarray(ambient, dtype=np.float64)
    nperseg = min(nperseg, len(b))
    f, cxy = signal.coherence(b, a, fs=fs, nperseg=nperseg)
    out = {}
    for name, (lo, hi) in _BANDS.items():
        m = (f >= lo) & (f <= hi)
        if not m.any():
            out[name] = {"mean": float("nan"), "max": float("nan"),
                         "f_at_max": float("nan")}
            continue
        band = cxy[m]
        bf = f[m]
        i = int(np.argmax(band))
        out[name] = {
            "mean": float(band.mean()),
            "max": float(band[i]),
            "f_at_max": float(bf[i]),
        }
    return out


def cycle_fidelity(x: np.ndarray, fs: int = 16000,
                   band_lo: float = 20.0, band_hi: float = 200.0) -> dict:
    """Cardiac-cycle fidelity of a single channel (non-tautological).

    Returns:
        autocorr_sigma : autocorr-peak strength in σ (≥3 = clear periodicity)
        autocorr_bpm   : BPM implied by that peak
        hr_bpm         : BPM from time-domain envelope peak detector
        hr_cv          : CV of inter-beat intervals (lower = more regular)
        n_peaks        : number of detected beats
        hr_confidence  : peak-count confidence (5/(1+4·CV))
    """
    sigma, ac_bpm = nc.hr_periodicity(x, fs=fs, band_lo=band_lo, band_hi=band_hi)
    bpm, conf, info = nc.hr_peak_count(x, fs=fs, band_lo=band_lo, band_hi=band_hi)
    return {
        "autocorr_sigma": float(sigma),
        "autocorr_bpm": float(ac_bpm),
        "hr_bpm": float(bpm),
        "hr_cv": float(info.get("cv", float("nan"))),
        "n_peaks": int(info.get("n_peaks", 0)),
        "hr_confidence": float(conf),
    }


def evaluate_anc(body: np.ndarray, ambient: np.ndarray, fs: int = 16000,
                 n_taps: int = 512, mu: float = 0.1,
                 band_lo: float = 20.0, band_hi: float = 200.0) -> dict:
    """Run NLMS(ambient → body) and compare honest metrics before vs after.

    Returns a dict with coherence, before/after fidelity, the (tautological,
    context-only) heart_snr_db pair, and a derived verdict.
    """
    body = np.asarray(body, dtype=np.float64)
    ambient = np.asarray(ambient, dtype=np.float64)

    coh = coherence_by_band(body, ambient, fs=fs)

    # NLMS-subtract ambient leakage from body. Compare in the SAME band so the
    # fidelity numbers are apples-to-apples (raw body band-passed vs cleaned).
    cleaned = nc.nlms_subtract(ambient, body, n_taps=n_taps, mu=mu)
    body_bp = nc.heart_filter(body, fs=fs, band_lo=band_lo, band_hi=band_hi)
    cleaned_bp = nc.heart_filter(cleaned, fs=fs, band_lo=band_lo, band_hi=band_hi)

    fid_before = cycle_fidelity(body_bp, fs=fs, band_lo=band_lo, band_hi=band_hi)
    fid_after = cycle_fidelity(cleaned_bp, fs=fs, band_lo=band_lo, band_hi=band_hi)

    # Context-only (tautological) — NOT used for the verdict.
    snr_before = nc.heart_snr_db(body, fs=fs, band_lo=band_lo, band_hi=band_hi)
    snr_after = nc.heart_snr_db(cleaned, fs=fs, band_lo=band_lo, band_hi=band_hi)

    verdict = _verdict(coh, fid_before, fid_after)

    return {
        "coherence": coh,
        "fidelity_before": fid_before,
        "fidelity_after": fid_after,
        "heart_snr_db_before": float(snr_before),  # tautological — context only
        "heart_snr_db_after": float(snr_after),     # tautological — context only
        "verdict": verdict,
        "params": {"n_taps": n_taps, "mu": mu,
                   "band": [band_lo, band_hi], "fs": fs},
    }


def _verdict(coh: dict, before: dict, after: dict) -> dict:
    """Decide whether ambient-ANC separates environment without harming signal.

    Two independent gates, both must pass for ANC to be 'safe-and-useful':
      A. Ambient must NOT carry the body signal in the heart/lung bands
         (mean in-band coherence below a threshold) — otherwise the reference
         contains the very signal we keep.
      B. Cardiac periodicity must survive NLMS: autocorr σ not materially
         reduced AND inter-beat CV not materially worsened.
    """
    HEART_COH_HI = 0.5   # ambient carries body signal if mean coh in heart band
    LUNG_COH_HI = 0.5    #   (or lung band) exceeds this
    SIGMA_DROP_FRAC = 0.20   # >20% σ loss = periodicity damaged
    CV_RISE_FRAC = 0.30      # >30% CV rise = beat regularity damaged

    heart_coh = coh["heart_20_150"]["mean"]
    lung_coh = coh["lung_150_1000"]["mean"]
    ambient_carries_signal = (heart_coh > HEART_COH_HI) or (lung_coh > LUNG_COH_HI)

    s_before, s_after = before["autocorr_sigma"], after["autocorr_sigma"]
    sigma_ok = s_after >= (1.0 - SIGMA_DROP_FRAC) * s_before if s_before > 0 else False
    cv_before, cv_after = before["hr_cv"], after["hr_cv"]
    # If CV is nan (too few beats), treat as a fidelity failure.
    if not np.isfinite(cv_before) or not np.isfinite(cv_after) or cv_before <= 0:
        cv_ok = False
    else:
        cv_ok = cv_after <= (1.0 + CV_RISE_FRAC) * cv_before
    fidelity_ok = sigma_ok and cv_ok

    safe_and_useful = (not ambient_carries_signal) and fidelity_ok

    reasons = []
    if ambient_carries_signal:
        reasons.append(
            f"ambient carries body signal in-band "
            f"(heart coh {heart_coh:.2f}, lung coh {lung_coh:.2f} ≥ {HEART_COH_HI}) "
            f"→ ANC reference contains the signal, will subtract it")
    if not sigma_ok:
        reasons.append(
            f"autocorr periodicity dropped {s_before:.2f}σ → {s_after:.2f}σ "
            f"(>{int(SIGMA_DROP_FRAC*100)}% loss)")
    if not cv_ok:
        reasons.append(
            f"inter-beat CV worsened {cv_before:.3f} → {cv_after:.3f}")
    if safe_and_useful:
        reasons.append("low in-band coherence + cardiac periodicity preserved")

    return {
        "safe_and_useful": bool(safe_and_useful),
        "ambient_carries_signal": bool(ambient_carries_signal),
        "fidelity_preserved": bool(fidelity_ok),
        "reasons": reasons,
    }


# ----------------------------- reporting -----------------------------------

def format_report(res: dict) -> str:
    L = []
    p = res["params"]
    L.append(f"NLMS params: n_taps={p['n_taps']} mu={p['mu']} "
             f"band={p['band'][0]:.0f}-{p['band'][1]:.0f} Hz @ {p['fs']} Hz")
    L.append("")
    L.append("Inter-mic coherence (body vs ambient) — 0=independent, 1=ambient predicts body:")
    for name, d in res["coherence"].items():
        L.append(f"  {name:18s} mean={d['mean']:.3f}  max={d['max']:.3f} "
                 f"@ {d['f_at_max']:.0f} Hz")
    L.append("")
    L.append("Cardiac cycle fidelity (body-raw → NLMS-cleaned), heart band:")
    b, a = res["fidelity_before"], res["fidelity_after"]
    L.append(f"  autocorr_sigma   {b['autocorr_sigma']:6.2f}σ → {a['autocorr_sigma']:6.2f}σ"
             f"   (≥3σ = clear periodicity; want preserved/up)")
    L.append(f"  hr_bpm           {b['hr_bpm']:6.1f}  → {a['hr_bpm']:6.1f}")
    L.append(f"  hr_cv            {b['hr_cv']:6.3f}  → {a['hr_cv']:6.3f}"
             f"   (lower = more regular beats)")
    L.append(f"  n_peaks          {b['n_peaks']:6d}  → {a['n_peaks']:6d}")
    L.append("")
    L.append(f"  [context only — TAUTOLOGICAL, do not use as a measure] "
             f"heart_snr_db {res['heart_snr_db_before']:+.1f} → "
             f"{res['heart_snr_db_after']:+.1f} dB")
    L.append("")
    v = res["verdict"]
    head = "✅ ANC SAFE & USEFUL" if v["safe_and_useful"] else "❌ ANC NOT SAFE / NOT USEFUL"
    L.append(f"VERDICT: {head}")
    L.append(f"  ambient carries body signal in-band: {v['ambient_carries_signal']}")
    L.append(f"  cardiac fidelity preserved:          {v['fidelity_preserved']}")
    for r in v["reasons"]:
        L.append(f"  - {r}")
    return "\n".join(L)


def _load_body_ambient(session: Path, channels: str,
                       start_sec: Optional[float], end_sec: Optional[float]):
    """Read audio.wav, resolve channel era → (fs, body, ambient) float64,
    sliced to [start_sec, end_sec)."""
    sr, stereo = _read_wav(session / "audio.wav")
    if stereo.ndim != 2 or stereo.shape[1] != 2:
        raise ValueError(f"expected stereo audio, got shape {stereo.shape}")

    legacy = (channels == "legacy")

    # body-first
    if legacy:
        body, ambient = stereo[:, 1], stereo[:, 0]
    else:
        body, ambient = stereo[:, 0], stereo[:, 1]

    i0 = int((start_sec or 0.0) * sr)
    i1 = int(end_sec * sr) if end_sec is not None else len(body)
    body = body[i0:i1].astype(np.float64)
    ambient = ambient[i0:i1].astype(np.float64)
    return sr, body, ambient, legacy


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session", type=Path, help="session folder (has audio.wav)")
    ap.add_argument("--start-sec", type=float, default=None)
    ap.add_argument("--end-sec", type=float, default=None)
    ap.add_argument("--n-taps", type=int, default=512)
    ap.add_argument("--mu", type=float, default=0.1)
    ap.add_argument("--band-lo", type=float, default=20.0)
    ap.add_argument("--band-hi", type=float, default=200.0)
    ap.add_argument("--channels", choices=("fixed", "legacy"),
                    default="fixed",
                    help="PDM channel order. 'fixed' (default) = col0 body / "
                         "col1 ambient (firmware with the 2026-06-08 PDM L/R "
                         "fix); 'legacy' = swapped.")
    args = ap.parse_args()

    sr, body, ambient, legacy = _load_body_ambient(
        args.session, args.channels, args.start_sec, args.end_sec)
    dur = len(body) / sr
    print(f"session: {args.session}")
    print(f"channel era: {'LEGACY (swapped)' if legacy else 'fixed (as-is)'} "
          f"[--channels {args.channels}]")
    win = (f"{args.start_sec or 0:.1f}–"
           f"{args.end_sec if args.end_sec is not None else dur + (args.start_sec or 0):.1f} s")
    print(f"window: {win}  ({dur:.1f} s, {len(body):,} samples/ch @ {sr} Hz)")
    print(f"  body    rms={np.sqrt(np.mean(body**2)):.1f}  "
          f"ambient rms={np.sqrt(np.mean(ambient**2)):.1f}")
    print()

    res = evaluate_anc(body, ambient, fs=sr,
                       n_taps=args.n_taps, mu=args.mu,
                       band_lo=args.band_lo, band_hi=args.band_hi)
    print(format_report(res))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
