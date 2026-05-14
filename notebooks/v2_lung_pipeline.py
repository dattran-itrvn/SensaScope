"""Lung pipeline: NLMS noise-cancel for lung-band 60-2000 Hz.

For lung sounds the bandpass overlaps voice (300-3000 Hz), so simple
20-200 Hz heart filter doesn't apply — voice will leak through. This is
where the ambient mic should pay off (ablation predicted +10 dB for
lung-band).

Run on last 100 s of SESSION_00002 (structured-protocol region) and on
the earlier 'sitting + chat' region. Compare:
  - Lung-band SNR before vs after NLMS
  - Voice-band energy reduction (speech rejection test)
  - Spectrograms before/after
  - Export cleaned WAVs for listen test
"""
from __future__ import annotations

import sys
import wave
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session
from tools.noise_cancel import nlms_subtract

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00002")
OUT = REPO / "notebooks" / "figures"
DOC = REPO / "notebooks" / "doctor_audio"
DOC.mkdir(exist_ok=True)

s = load_session(SESSION)
audio = s["audio"]; fs = s["fs_audio"]
body = audio[:, 0].astype(np.float32) / 32768
amb  = audio[:, 1].astype(np.float32) / 32768
dur = len(body) / fs
print(f"session 2: {dur:.1f}s")

# Filters
sos_lung  = signal.butter(4, [60, 2000], btype="band", fs=fs, output="sos")
sos_voice = signal.butter(4, [300, 3000], btype="band", fs=fs, output="sos")
sos_heart = signal.butter(4, [20, 200], btype="band", fs=fs, output="sos")

def band_rms(x, sos):
    bp = signal.sosfiltfilt(sos, x)
    return float(np.sqrt(np.mean(bp**2)))

def save_wav(path, x, fs_out):
    peak = np.abs(x).max() + 1e-9
    data = ((x / peak) * 0.95 * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(fs_out)
        w.writeframes(data.tobytes())

# %% Two regions
WINDOWS = [
    ("01_chat_sitting", 180, 240),     # user said "có nói chuyện" — chat phase
    ("02_structured",   275, 350),     # protocol phases at the end
]

print(f"\n{'window':<22} {'sec':>5}  "
      f"{'raw lung':>9} {'clean lung':>11} Δ  "
      f"{'raw voice':>10} {'clean voice':>11} Δ  rejection")
for name, t0, t1 in WINDOWS:
    n0, n1 = int(t0 * fs), int(t1 * fs)
    body_w = body[n0:n1]
    amb_w  = amb[n0:n1]

    # Pipeline: NLMS subtract ambient, then keep lung band
    cleaned_raw = nlms_subtract(amb_w, body_w, n_taps=512, mu=0.1)
    cleaned_lung = signal.sosfiltfilt(sos_lung, cleaned_raw)

    # Metrics: lung-band RMS (signal) and voice-band RMS (interference)
    raw_lung    = band_rms(body_w, sos_lung)
    raw_voice   = band_rms(body_w, sos_voice)
    clean_lung  = float(np.sqrt(np.mean(cleaned_lung**2)))
    clean_voice = band_rms(cleaned_raw, sos_voice)

    d_lung_db  = 20 * np.log10(clean_lung / (raw_lung + 1e-12))
    d_voice_db = 20 * np.log10(clean_voice / (raw_voice + 1e-12))
    rejection_db = -d_voice_db
    print(f"{name:<22} {t1-t0:>5d}  "
          f"{20*np.log10(raw_lung+1e-12):>+9.2f} {20*np.log10(clean_lung+1e-12):>+11.2f} {d_lung_db:>+6.2f}  "
          f"{20*np.log10(raw_voice+1e-12):>+10.2f} {20*np.log10(clean_voice+1e-12):>+11.2f} {d_voice_db:>+6.2f}  "
          f"voice rejected {rejection_db:+5.1f} dB")

    save_wav(DOC / f"lung_{name}_raw_body.wav", body_w, fs)
    save_wav(DOC / f"lung_{name}_cleaned.wav", cleaned_lung, fs)

# %% Detailed view: structured 75-s window
T0, T1 = 275, 350
n0, n1 = int(T0 * fs), int(T1 * fs)
body_z = body[n0:n1]; amb_z = amb[n0:n1]
cleaned_raw = nlms_subtract(amb_z, body_z, n_taps=512, mu=0.1)
cleaned_lung = signal.sosfiltfilt(sos_lung, cleaned_raw)

fig, axs = plt.subplots(3, 1, figsize=(15, 9), sharex=True)

for ax, sig_, name, cmap in [
    (axs[0], body_z,         "RAW body mic",          "viridis"),
    (axs[1], cleaned_raw,    "AFTER NLMS (full band)", "viridis"),
    (axs[2], cleaned_lung,   "AFTER NLMS + 60-2000 Hz bandpass (final lung output)", "viridis"),
]:
    f_, ts_, S_ = signal.spectrogram(sig_, fs=fs, nperseg=2048, noverlap=1024)
    mask = f_ <= 3000
    p = 10 * np.log10(np.maximum(S_[mask], 1e-12))
    pcm = ax.pcolormesh(ts_ + T0, f_[mask], p, shading="auto", cmap=cmap,
                          vmin=p.max() - 60, vmax=p.max())
    ax.set_ylabel("freq (Hz)")
    ax.set_title(name)
    ax.axhline(2000, color="white", ls="--", lw=0.5, alpha=0.5)
    ax.axhline(300, color="cyan", ls=":", lw=0.5, alpha=0.6)
    plt.colorbar(pcm, ax=ax, label="dB")
axs[-1].set_xlabel("time (s)")
plt.tight_layout()
plt.savefig(OUT / "lung_pipeline_75s.png", dpi=110)
plt.close()
print(f"\nsaved {OUT / 'lung_pipeline_75s.png'}")

save_wav(DOC / "lung_structured_75s_raw.wav",     body_z, fs)
save_wav(DOC / "lung_structured_75s_cleaned.wav", cleaned_lung, fs)
print(f"saved doctor-audio WAVs for 75-s structured window in {DOC}/")
