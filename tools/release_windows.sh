#!/usr/bin/env bash
# Builds a self-contained Windows release of Mudslide and, optionally,
# uploads it to itch.io with butler.
#
# The game is linked against static SDL2 and the static C++ runtime
# (vcpkg triplet x64-windows-static), so players need no DLLs or Visual C++
# redistributable: the release is one mudslide.exe plus a readme and licenses.
#
# Steps: configure and build build-release/, run the tests, check the .exe
# imports no SDL or VC++ runtime DLLs, stage dist/mudslide-<version>-windows/,
# zip it to dist/mudslide-<version>-windows.zip, and, when an itch.io target
# is given, push the staged folder to its "windows" channel.
#
# Usage: tools/release_windows.sh [itch-user/game]
#   tools/release_windows.sh                    # build, test, package only
#   tools/release_windows.sh someone/mudslide   # ... and upload with butler
#
# Needs Visual Studio 2022, vcpkg (VCPKG_ROOT), and for uploading butler on
# the PATH, logged in (run "butler login" once).
set -euo pipefail

cd "$(dirname "$0")/.."
TARGET=${1:-}
VERSION=$(sed -n 's/^project(Mudslide VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
if [[ -z "$VERSION" ]]; then
  echo "Could not read the version from CMakeLists.txt" >&2
  exit 1
fi
: "${VCPKG_ROOT:?set VCPKG_ROOT to your vcpkg checkout}"
BUILD=build-release
NAME=mudslide-$VERSION-windows
STAGE=dist/$NAME

echo "== Building Mudslide $VERSION (x64, static)"
cmake -S . -B "$BUILD" -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>' >/dev/null
cmake --build "$BUILD" --config Release -- -nologo -verbosity:minimal

echo "== Testing"
ctest --test-dir "$BUILD" -C Release --output-on-failure

EXE=$BUILD/Release/mudslide.exe
for dll in SDL2.dll VCRUNTIME140.dll MSVCP140.dll; do
  if grep -qai "$dll" "$EXE"; then
    echo "$EXE still imports $dll - the release would not run on a clean machine" >&2
    exit 1
  fi
done

echo "== Staging $STAGE"
rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE/licenses"
cp "$EXE" "$STAGE/"
sed "s/{{VERSION}}/$VERSION/g" tools/release/README.txt > "$STAGE/README.txt"
cp tools/release/itch.toml "$STAGE/.itch.toml"
cp LICENSE "$STAGE/licenses/Mudslide-GPL-3.0.txt"
cp "$BUILD/vcpkg_installed/x64-windows-static/share/sdl2/copyright" "$STAGE/licenses/SDL2.txt"

echo "== Zipping dist/$NAME.zip"
# Windows' own bsdtar writes a standard zip; Git Bash's tar cannot.
TAR=tar
[[ -x /c/Windows/System32/tar.exe ]] && TAR=/c/Windows/System32/tar.exe
(cd dist && "$TAR" -a -cf "$NAME.zip" "$NAME")
ls -l "dist/$NAME.zip"

if [[ -n "$TARGET" ]]; then
  echo "== Uploading to itch.io: $TARGET:windows ($VERSION)"
  butler push "$STAGE" "$TARGET:windows" --userversion "$VERSION"
  butler status "$TARGET:windows"
fi
