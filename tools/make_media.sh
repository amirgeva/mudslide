#!/usr/bin/env bash
# Records the gameplay clip and encodes docs/gameplay.mp4 (1280x720, 30 fps)
# and docs/gameplay.gif (800 px, 15 fps), then renders the cover image
# docs/cover.png (1280x640, GitHub's social preview size).
# Needs ffmpeg (with libx264) on the PATH.
#
# Usage: tools/make_media.sh [path/to/mudslide]
#   defaults to build/Release/mudslide (Visual Studio) or build/mudslide
set -euo pipefail

cd "$(dirname "$0")/.."
GAME=${1:-}
if [[ -z "$GAME" ]]; then
  if [[ -x build/Release/mudslide || -x build/Release/mudslide.exe ]]; then
    GAME=build/Release/mudslide
  else
    GAME=build/mudslide
  fi
fi

W=1280
H=720
FPS=30
GIF_W=800
GIF_FPS=15
OUT=docs
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

echo "Recording gameplay..."
"$GAME" --record - --size "${W}x${H}" --fps "$FPS" |
  ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s "${W}x${H}" -r "$FPS" -i - \
    -c:v ffv1 "$WORK/clip.mkv"

echo "Encoding $OUT/gameplay.mp4..."
ffmpeg -y -loglevel error -i "$WORK/clip.mkv" \
  -c:v libx264 -preset slow -crf 20 -pix_fmt yuv420p -movflags +faststart "$OUT/gameplay.mp4"

echo "Encoding $OUT/gameplay.gif..."
ffmpeg -y -loglevel error -i "$WORK/clip.mkv" \
  -vf "fps=${GIF_FPS},scale=${GIF_W}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=160:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" \
  -loop 0 "$OUT/gameplay.gif"

echo "Rendering $OUT/cover.png..."
"$GAME" --screenshot "$WORK/cover.bmp" --scene cover --size 1280x640
ffmpeg -y -loglevel error -i "$WORK/cover.bmp" "$OUT/cover.png"

ls -l "$OUT/gameplay.mp4" "$OUT/gameplay.gif" "$OUT/cover.png"
