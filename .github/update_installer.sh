#!/bin/bash
# Refresh the web installer (docs/) for a release: copy the factory images from <dist-dir> over
# docs/firmware/*.bin and set the version label in docs/index.html. Run from anywhere in the repo.
# Usage: .github/update_installer.sh v1.6.7 dist
set -e
TAG="${1:?usage: update_installer.sh <tag> <dist-dir>}"
DIST="${2:?usage: update_installer.sh <tag> <dist-dir>}"
cd "$(git rev-parse --show-toplevel)"

# build env : installer file name (docs/firmware/<name>.bin)
for pair in freenove4:aquarium freenove28:aquarium-freenove28 cyd28:aquarium-cyd28 cyd28usb2:aquarium-cyd28-2usb; do
  env="${pair%%:*}"
  name="${pair##*:}"
  cp "$DIST/Fish-Tank-$env-factory.bin" "docs/firmware/$name.bin"
done

# the four board choices ("&mdash; vX.Y.Z") and the "Version vX.Y.Z" line
sed -i -E "/<option value=/ s/&mdash; v[0-9]+(\.[0-9]+)*/\&mdash; $TAG/; s/Version <strong>v[0-9]+(\.[0-9]+)*<\/strong>/Version <strong>$TAG<\/strong>/" docs/index.html

n="$(grep -o "$TAG" docs/index.html | wc -l)"
[ "$n" -eq 5 ] || { echo "expected 5 version labels in docs/index.html, found $n" >&2; exit 1; }
echo "installer updated for $TAG"
