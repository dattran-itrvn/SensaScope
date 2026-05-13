#!/usr/bin/env python3
"""
Concatenate audio.wav from all SESSION_NNNNN folders into a single WAV
for end-to-end listening.

Skips folders that lack the .unsynced marker (incomplete sessions) unless
--include-incomplete is given. Sessions are concatenated in numeric order.

Usage:
    python tools/concat_audio.py <sd_root>
    python tools/concat_audio.py <sd_root> -o merged.wav
    python tools/concat_audio.py <sd_root> --include-incomplete
"""
from __future__ import annotations

import argparse
import re
import sys
import wave
from pathlib import Path

SESSION_RE = re.compile(r"^SESSION_(\d{5})$")


def find_sessions(sd_root: Path) -> list[Path]:
	out = []
	for p in sd_root.iterdir():
		if p.is_dir() and SESSION_RE.match(p.name):
			out.append(p)
	out.sort(key=lambda p: int(SESSION_RE.match(p.name).group(1)))
	return out


def session_status(folder: Path) -> tuple[bool, str]:
	"""Return (is_complete, reason). Complete = has audio.wav AND .unsynced."""
	audio = folder / "audio.wav"
	marker = folder / ".unsynced"
	if not audio.is_file():
		return False, "no audio.wav"
	if audio.stat().st_size < 44:
		return False, f"audio.wav too small ({audio.stat().st_size} B)"
	if not marker.is_file():
		return False, "no .unsynced marker"
	return True, "ok"


def concat(folders: list[Path], output: Path) -> None:
	if not folders:
		print("nothing to merge", file=sys.stderr)
		sys.exit(1)

	first = folders[0] / "audio.wav"
	with wave.open(str(first), "rb") as wf:
		nch = wf.getnchannels()
		sw = wf.getsampwidth()
		fr = wf.getframerate()

	total_frames = 0
	with wave.open(str(output), "wb") as out:
		out.setnchannels(nch)
		out.setsampwidth(sw)
		out.setframerate(fr)

		for folder in folders:
			path = folder / "audio.wav"
			try:
				with wave.open(str(path), "rb") as wf:
					if (wf.getnchannels(), wf.getsampwidth(),
					    wf.getframerate()) != (nch, sw, fr):
						print(f"WARN: {folder.name} format mismatch, "
						      f"skipping", file=sys.stderr)
						continue
					n = wf.getnframes()
					data = wf.readframes(n)
					out.writeframes(data)
					total_frames += n
					print(f"  + {folder.name}: {n} frames "
					      f"({n/fr:.1f} s)")
			except wave.Error as e:
				print(f"WARN: {folder.name} unreadable ({e}), skipping",
				      file=sys.stderr)

	dur = total_frames / fr
	print(f"\nMerged {len(folders)} sessions → {output}")
	print(f"  format : {nch} ch, {sw * 8}-bit, {fr} Hz")
	print(f"  frames : {total_frames}")
	print(f"  duration: {dur:.1f} s ({dur/60:.2f} min)")


def main() -> None:
	ap = argparse.ArgumentParser(description=__doc__,
	                             formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("sd_root", type=Path,
	                help="path to SD card root (or a local copy)")
	ap.add_argument("-o", "--output", type=Path, default=None,
	                help="output WAV (default: <sd_root>/merged.wav)")
	ap.add_argument("--include-incomplete", action="store_true",
	                help="also include folders missing .unsynced marker")
	args = ap.parse_args()

	if not args.sd_root.is_dir():
		print(f"not a directory: {args.sd_root}", file=sys.stderr)
		sys.exit(1)

	all_folders = find_sessions(args.sd_root)
	if not all_folders:
		print("no SESSION_* folders found", file=sys.stderr)
		sys.exit(1)

	complete: list[Path] = []
	skipped: list[tuple[Path, str]] = []
	for f in all_folders:
		ok, reason = session_status(f)
		if ok or args.include_incomplete:
			complete.append(f)
		else:
			skipped.append((f, reason))

	if skipped:
		print(f"skipping {len(skipped)} incomplete session(s):", file=sys.stderr)
		for f, r in skipped:
			print(f"  - {f.name} ({r})", file=sys.stderr)

	output = args.output or (args.sd_root / "merged.wav")
	concat(complete, output)


if __name__ == "__main__":
	main()
