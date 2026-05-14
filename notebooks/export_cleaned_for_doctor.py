"""Export cleaned heart audio — what would be sent over BLE to the doctor.

For each posture-stable segment in SESSION_00001, run the full v2 pipeline
and save:
  1. 16 kHz mono cleaned WAV — full-quality reference (for analysis)
  2. 4 kHz mono cleaned WAV — production-format proxy
     (Nyquist 2 kHz covers heart 20-200 Hz with margin; data rate 8 kB/s,
      half of macOS BLE 17 kB/s ceiling — comfortable real-time stream)
  3. One combined "all_postures_cleaned.wav" — full session in production format

Audio output is purely heart-band content. Voice / environmental sounds /
out-of-band noise are removed by the NLMS + 20-200 Hz bandpass pipeline.
"""
# %% imports + load
from __future__ import annotations

import sys
import wave
from pathlib import Path

import numpy as np
from scipy import signal

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.load_session import load_session
from tools.noise_cancel import pipeline, hr_peak_count

SESSION = Path("/Volumes/SENSAPULSE/SESSION_00001")
OUT = REPO / "notebooks" / "doctor_audio"
OUT.mkdir(parents=True, exist_ok=True)

LABELS = [
    "01_dung",  "02_nam",  "03_nghieng_phai", "04_nghieng_trai", "05_nam_thang",
    "06_nghieng_phai", "07_nghieng_trai", "08_nam_thang", "09_dung", "10_ngoi",
]

s = load_session(SESSION)
audio = s["audio"]
fs    = s["fs_audio"]
imu   = s["imu"]
body = audio[:, 0].astype(np.float32) / 32768.0
amb  = audio[:, 1].astype(np.float32) / 32768.0
print(f"loaded {len(body)/fs:.1f}s @ {fs} Hz")

# %% IMU segmentation (same as v2_postures.py)
G_LSB = 1.0 / 16384.0
gx = signal.sosfiltfilt(signal.butter(2, 0.5, "low", fs=52, output="sos"),
                        imu.ax.values * G_LSB)
gy = signal.sosfiltfilt(signal.butter(2, 0.5, "low", fs=52, output="sos"),
                        imu.ay.values * G_LSB)
gz = signal.sosfiltfilt(signal.butter(2, 0.5, "low", fs=52, output="sos"),
                        imu.az.values * G_LSB)
g_mag = np.sqrt(gx**2 + gy**2 + gz**2)
gx_u, gy_u, gz_u = gx / g_mag, gy / g_mag, gz / g_mag
dot = np.clip(gx_u[1:]*gx_u[:-1] + gy_u[1:]*gy_u[:-1] + gz_u[1:]*gz_u[:-1], -1, 1)
ang_vel = np.concatenate([[0], np.degrees(np.arccos(dot)) * 52])
ang_vel_s = signal.savgol_filter(ang_vel, 21, 2)
imu_t = (imu.t_us.values - imu.t_us.iloc[0]) / 1e6

is_stable = ang_vel_s < 5.0
min_len = int(15.0 * 52)
runs = []
i = 0
while i < len(is_stable):
    if is_stable[i]:
        j = i
        while j < len(is_stable) and is_stable[j]: j += 1
        if j - i >= min_len: runs.append((i, j))
        i = j
    else:
        i += 1
print(f"detected {len(runs)} posture plateaus")

# %% Helpers
def save_wav_16bit(path: Path, x: np.ndarray, fs: int):
    """Save mono 16-bit WAV, auto-normalize to ±0.95 to avoid clipping."""
    peak = np.abs(x).max() + 1e-9
    data = ((x / peak) * 0.95 * 32767).astype(np.int16)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(fs)
        w.writeframes(data.tobytes())

# Downsampler 16 kHz → 4 kHz (decimate 4x with anti-alias)
def downsample_4x(x: np.ndarray) -> np.ndarray:
    return signal.decimate(x, 4, ftype="iir", zero_phase=True).astype(np.float32)

# %% Process each segment, save WAVs, track HR
all_cleaned_16k = []
print(f"\n{'#':>3}  {'label':<20}  {'sec':>5}  {'BPM':>5}  files")
for k, (i0, i1) in enumerate(runs):
    t0_s = imu_t[i0] + 5.0           # skip first 5s for NLMS converge
    t1_s = imu_t[i1 - 1]
    if t1_s - t0_s < 10.0:
        continue
    label = LABELS[k] if k < len(LABELS) else f"unknown_{k}"
    n0, n1 = int(t0_s * fs), int(t1_s * fs)

    cleaned_16k = pipeline(body[n0:n1], amb[n0:n1], fs=fs, n_taps=512, mu=0.1)
    cleaned_4k = downsample_4x(cleaned_16k)
    bpm, conf, info = hr_peak_count(cleaned_16k, fs)

    # Save 16 kHz reference + 4 kHz production-proxy
    name_16k = f"{label}_16kHz.wav"
    name_4k  = f"{label}_4kHz.wav"
    save_wav_16bit(OUT / name_16k, cleaned_16k, fs)
    save_wav_16bit(OUT / name_4k,  cleaned_4k,  4000)
    all_cleaned_16k.append(cleaned_16k)

    sec = t1_s - t0_s
    print(f"{k+1:>3}  {label:<20}  {sec:>5.1f}  {bpm:>5.1f}  {name_16k}, {name_4k}")

# %% Combined "all postures" file in production format (4 kHz mono)
combined_16k = np.concatenate(all_cleaned_16k)
combined_4k = downsample_4x(combined_16k)
save_wav_16bit(OUT / "ALL_postures_4kHz.wav", combined_4k, 4000)
save_wav_16bit(OUT / "ALL_postures_16kHz.wav", combined_16k, 16000)

# %% Data-rate summary
size_4k = len(combined_4k) * 2     # 16-bit mono
size_16k = len(combined_16k) * 2
dur = len(combined_16k) / fs
print(f"\n=== output summary ===")
print(f"total duration:  {dur:.1f}s ({dur/60:.1f} min)")
print(f"16 kHz mono WAV: {size_16k/1024:.0f} KB ({size_16k/dur/1024:.1f} KB/s rate)")
print(f" 4 kHz mono WAV: {size_4k/1024:.0f} KB ({size_4k/dur/1024:.1f} KB/s rate)")
print(f"Apple BLE ceiling: ~17 KB/s → 4 kHz mono fits with ~50% margin")
print(f"saved {2 + 2*len(runs)} WAV files to {OUT}/")

# %% Comparison: raw vs cleaned (for doctor to A/B if requested)
raw_body_combined = np.concatenate([body[int((imu_t[i0]+5)*fs):int(imu_t[i1-1]*fs)]
                                     for (i0, i1) in runs
                                     if imu_t[i1-1] - imu_t[i0] - 5 >= 10])
save_wav_16bit(OUT / "ALL_postures_RAW_body.wav", raw_body_combined, 16000)
print(f"also saved RAW body for A/B comparison")
