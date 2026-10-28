#!/usr/bin/env bash
# Webcam pointed at the watch screen (our stand-in for UART).
#   cam.sh snap [name]           one frame        -> cam/<name>.jpg
#   cam.sh burst <secs> [name]   1 frame/second   -> cam/<name>-NNN.jpg (for watching a boot)
set -euo pipefail

CAM=${CAM:-"Microsoft® LifeCam HD-3000"}
OUT=$(dirname "$0")/../cam
mkdir -p "$OUT"
grab() { ffmpeg -hide_banner -loglevel error -f dshow -video_size 1280x720 -i video="$CAM" "$@"; }

case ${1:-snap} in
  snap)  grab -ss 1.5 -frames:v 1 -y "$OUT/${2:-snap}.jpg"; echo "$OUT/${2:-snap}.jpg" ;;
  burst) grab -t "${2:?seconds}" -vf fps=1 -y "$OUT/${3:-burst}-%03d.jpg"; ls "$OUT/${3:-burst}"-*.jpg | wc -l ;;
  *) echo "usage: $0 snap [name] | burst <secs> [name]" >&2; exit 2 ;;
esac
