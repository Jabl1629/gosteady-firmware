#!/usr/bin/env bash
# Load the Family Assistance prompt set onto a DFR0534 module.
#
# 1. Connect the module's micro-USB port to the Mac (it mounts as a small FAT
#    volume, usually under /Volumes/).
# 2. Run:  tools/load_dfr0534_prompts.sh "/Volumes/<name>"
#
# Track index = copy order, so files are removed first and copied ONE AT A TIME
# in numeric order as 8.3 names (01.wav … 10.wav). macOS AppleDouble (._*)
# sidecar files would be counted as tracks, so they are suppressed + cleaned.
set -euo pipefail
VOL="${1:?usage: $0 /Volumes/<DFR0534 volume>}"
SRC="$(cd "$(dirname "$0")/../audio/prompts" && pwd)"
[ -d "$VOL" ] || { echo "not a directory: $VOL"; exit 1; }

export COPYFILE_DISABLE=1
echo "Removing existing audio files on $VOL …"
find "$VOL" -maxdepth 1 -type f \( -iname '*.mp3' -o -iname '*.wav' -o -name '._*' \) -print -delete || true
sync
n=0
for f in $(ls "$SRC"/*.wav | sort); do
  base="$(basename "$f")"; idx="${base%%_*}"
  echo "copy $base → $idx.wav"
  cp "$f" "$VOL/$idx.wav"
  sync
  n=$((n+1))
done
dot_clean -m "$VOL" 2>/dev/null || true
find "$VOL" -maxdepth 1 -name '._*' -delete 2>/dev/null || true
echo "Copied $n files:"; ls -la "$VOL"
diskutil unmount "$VOL" || true
echo "Done. Power-cycle the module (the firmware does this on every incident)."
