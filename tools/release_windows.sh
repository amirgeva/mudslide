#!/usr/bin/env bash
# Builds a self-contained Windows release and, only with --upload, pushes it
# to itch.io with butler. Edit the CONFIG block for this game.
#
# The game is linked against static vcpkg libraries and the static C++
# runtime (triplet x64-windows-static), so players need no DLLs and no
# Visual C++ redistributable: the zip holds one .exe, a readme and licenses.
#
# Steps: configure + build build-release/, run the tests, check the .exe only
# imports system DLLs, stage dist/<exe>-<version>-windows/, zip it, and with
# --upload push the staged folder to the itch.io "windows" channel.
#
# Usage: tools/release_windows.sh                    build, test, package
#        tools/release_windows.sh --upload           ... and upload to tools/release/itch-target
#        tools/release_windows.sh --upload user/game ... and upload there
# Needs Visual Studio 2022, vcpkg (VCPKG_ROOT) and, to upload, butler logged in.
set -euo pipefail
cd "$(dirname "$0")/.."

# ---- CONFIG -------------------------------------------------------------
EXE_NAME=mudslide                                 # CMake executable target
LICENSE_FILE=LICENSE                              # shipped as licenses/<GAME_TITLE>-license.txt
GAME_TITLE=Mudslide
SOURCE_URL=https://github.com/amirgeva/mudslide   # put into README.txt ({{SOURCE_URL}})
# -------------------------------------------------------------------------

UPLOAD=0
TARGET=
if [[ ${1:-} == --upload ]]; then
  UPLOAD=1
  TARGET=${2:-}
  if [[ -z "$TARGET" && -f tools/release/itch-target ]]; then TARGET=$(tr -d ' \r\n' < tools/release/itch-target); fi
  if [[ -z "$TARGET" ]]; then
    echo "--upload needs user/game, or a tools/release/itch-target file" >&2
    exit 1
  fi
elif [[ -n ${1:-} ]]; then
  echo "usage: $0 [--upload [user/game]]" >&2
  exit 1
fi

VERSION=$(sed -n 's/^project([A-Za-z0-9_]* VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
if [[ -z "$VERSION" ]]; then
  echo "No version: add VERSION x.y.z to project() in CMakeLists.txt" >&2
  exit 1
fi
: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"
BUILD=build-release
NAME=$EXE_NAME-$VERSION-windows
STAGE=dist/$NAME

echo "== Building $GAME_TITLE $VERSION (x64, static)"
cmake -S . -B "$BUILD" -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>' >/dev/null
cmake --build "$BUILD" --config Release -- -nologo -verbosity:minimal

echo "== Testing"
ctest --test-dir "$BUILD" -C Release --output-on-failure --no-tests=ignore

EXE=$BUILD/Release/$EXE_NAME.exe
echo "== Checking $EXE imports only system DLLs"
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
DUMPBIN=
if [[ -x "$VSWHERE" ]]; then
  VS=$("$VSWHERE" -latest -property installationPath | tr -d '\r')
  DUMPBIN=$(ls "$(cygpath "$VS")"/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe 2>/dev/null | tail -1 || true)
fi
if [[ -n "$DUMPBIN" ]]; then
  DLLS=$("$DUMPBIN" -nologo -dependents "$EXE" | grep -i '\.dll' | tr -d ' \r')
  echo "$DLLS" | tr '\n' ' '; echo
  BAD=$(echo "$DLLS" | grep -iE '^(SDL|VCRUNTIME|MSVCP|CONCRT|libgcc|libstdc|libwinpthread)|_d\.dll$' || true)
else
  echo "(dumpbin not found; falling back to a string search)"
  BAD=$(for d in SDL2.dll VCRUNTIME140.dll MSVCP140.dll; do grep -qai "$d" "$EXE" && echo "$d"; done || true)
fi
if [[ -n "$BAD" ]]; then
  echo "$EXE needs: $BAD - it would not run on a clean machine" >&2
  exit 1
fi

echo "== Staging $STAGE"
rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE/licenses"
cp "$EXE" "$STAGE/"
sed -e "s/{{VERSION}}/$VERSION/g" -e "s|{{SOURCE_URL}}|$SOURCE_URL|g" tools/release/README.txt > "$STAGE/README.txt"
cp tools/release/itch.toml "$STAGE/.itch.toml"
cp "$LICENSE_FILE" "$STAGE/licenses/$GAME_TITLE-license.txt"
for c in "$BUILD"/vcpkg_installed/x64-windows-static/share/*/copyright; do
  [[ -f "$c" ]] && cp "$c" "$STAGE/licenses/$(basename "$(dirname "$c")").txt"
done

echo "== Zipping dist/$NAME.zip"
# Windows' own bsdtar writes a standard zip; Git Bash's tar cannot.
TAR=tar
[[ -x /c/Windows/System32/tar.exe ]] && TAR=/c/Windows/System32/tar.exe
(cd dist && "$TAR" -a -cf "$NAME.zip" "$NAME")
ls -l "dist/$NAME.zip"

if [[ $UPLOAD == 1 ]]; then
  echo "== Uploading to itch.io: $TARGET:windows ($VERSION)"
  butler push "$STAGE" "$TARGET:windows" --userversion "$VERSION"
  butler status "$TARGET:windows" || true
fi
