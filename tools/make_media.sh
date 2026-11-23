#!/usr/bin/env bash
# Publication media: gameplay clip (MP4 + GIF), store screenshots and cover
# images, all rendered offscreen by the game itself (deterministic: re-runs
# reproduce the same files). Edit the CONFIG block for this game.
#
#   docs/gameplay.mp4                  1280x720 H.264 clip
#   docs/gameplay.gif                  smaller GIF of the same recording
#   docs/store/screenshots/<name>.png  store screenshots
#   docs/cover.png                     1280x640 (GitHub social preview)
#   docs/store/itch-cover.png          630x500 (itch.io cover image)
#   dist/gameplay-sheet.png            contact sheet of the clip, for checking
#   dist/screenshots-sheet.png         all screenshots on one sheet, for checking
#
# Usage: tools/make_media.sh [path/to/game]
#   defaults to build/Release/<exe> (Visual Studio) or build/<exe>
# Needs bash and ffmpeg (with libx264) on the PATH.
set -euo pipefail
cd "$(dirname "$0")/.."

# ---- CONFIG -------------------------------------------------------------
EXE_NAME=mudslide            # CMake executable target
CLIP_SIZE=1280x720           # recording size (keep the UI scale readable)
FPS=30                       # recording and MP4 frame rate
GIF_W=800                    # GIF width; height keeps the aspect
GIF_FPS=15
RECORD=1                     # 0 if the game has no --record mode yet
# Store screenshots: "<file name>|<game arguments for --screenshot>"
SHOT_SIZE=1280x720
SHOTS=(
  "01-menu|--scene menu --seed 9"
  "02-aiming|--scene aim --human --players 2 --seed 9"
  "03-mud-ball|--scene flight --seed 2 --turns 6"
  "04-blast|--scene debris --seed 4 --turns 4"
  "05-buried|--scene aim --seed 3 --turns 14"
  "06-scores|--scene round --seed 7"
)
COVER_SCENE=(--scene cover)  # empty array: no cover scene, skip covers
# -------------------------------------------------------------------------

GAME=${1:-}
if [[ -z "$GAME" ]]; then
  if [[ -x build/Release/$EXE_NAME || -x build/Release/$EXE_NAME.exe ]]; then
    GAME=build/Release/$EXE_NAME
  else
    GAME=build/$EXE_NAME
  fi
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p docs/store/screenshots dist

if [[ $RECORD == 1 ]]; then
  W=${CLIP_SIZE%x*}
  echo "Recording gameplay..."
  "$GAME" --record - --size "$CLIP_SIZE" --fps "$FPS" |
    ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s "$CLIP_SIZE" -r "$FPS" -i - \
      -c:v ffv1 "$WORK/clip.mkv"

  echo "Encoding docs/gameplay.mp4..."
  ffmpeg -y -loglevel error -i "$WORK/clip.mkv" \
    -c:v libx264 -preset slow -crf 20 -pix_fmt yuv420p -movflags +faststart docs/gameplay.mp4

  echo "Encoding docs/gameplay.gif..."
  ffmpeg -y -loglevel error -i "$WORK/clip.mkv" \
    -vf "fps=${GIF_FPS},scale=${GIF_W}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=160:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" \
    -loop 0 docs/gameplay.gif

  # One frame every 2.5 s, 3 across, to eyeball the clip.
  ffmpeg -y -loglevel error -i "$WORK/clip.mkv" \
    -vf "fps=0.4,scale=$((W / 3)):-1,tile=3x5" -frames:v 1 dist/gameplay-sheet.png
fi

: > "$WORK/shots.txt"
for shot in "${SHOTS[@]}"; do
  name=${shot%%|*}
  read -r -a args <<< "${shot#*|}"
  echo "Screenshot $name..."
  "$GAME" --screenshot "$WORK/$name.bmp" --size "$SHOT_SIZE" "${args[@]}" >/dev/null 2>&1
  ffmpeg -y -loglevel error -i "$WORK/$name.bmp" "docs/store/screenshots/$name.png"
  echo "file '$(cygpath -m "$PWD/docs/store/screenshots/$name.png" 2>/dev/null || echo "$PWD/docs/store/screenshots/$name.png")'" >> "$WORK/shots.txt"
done
if [[ ${#SHOTS[@]} -gt 0 ]]; then
  # All screenshots on one sheet, two across, to eyeball them together.
  ffmpeg -y -loglevel error -f concat -safe 0 -i "$WORK/shots.txt" \
    -vf "scale=640:-1,tile=2x$(( (${#SHOTS[@]} + 1) / 2 ))" -frames:v 1 dist/screenshots-sheet.png
fi

if [[ ${#COVER_SCENE[@]} -gt 0 ]]; then
  echo "Rendering covers..."
  "$GAME" --screenshot "$WORK/cover.bmp" --size 1280x640 "${COVER_SCENE[@]}" >/dev/null 2>&1
  ffmpeg -y -loglevel error -i "$WORK/cover.bmp" docs/cover.png
  "$GAME" --screenshot "$WORK/itch.bmp" --size 630x500 "${COVER_SCENE[@]}" >/dev/null 2>&1
  ffmpeg -y -loglevel error -i "$WORK/itch.bmp" docs/store/itch-cover.png
fi

ls -l docs/gameplay.* docs/cover.png docs/store/itch-cover.png docs/store/screenshots/*.png 2>/dev/null
size() { stat -c %s "$1" 2>/dev/null || echo 0; }
if [[ -f docs/gameplay.gif && $(size docs/gameplay.gif) -gt 8000000 ]]; then
  echo "warning: gameplay.gif is over 8 MB; shorten the clip or lower GIF_W / GIF_FPS" >&2
fi
if [[ -f docs/store/itch-cover.png && $(size docs/store/itch-cover.png) -gt 3000000 ]]; then
  echo "warning: itch-cover.png is over itch's 3 MB limit" >&2
fi
