#!/bin/bash
# Assembles build/package/Faceit_RoundEndStats: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/Faceit_RoundEndStats"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/Faceit_RoundEndStats/libFaceit_RoundEndStats.so" "$OUT/bin/"
cp "$ROOT/build/plugins/Faceit_RoundEndStats/Faceit_RoundEndStats.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
