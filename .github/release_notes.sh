#!/bin/bash
# Release body for a tag, on stdout: the fixed install help (release-template.md) plus a
# "What's new" section taken from the tag's annotation (its message). A lightweight tag
# falls back to the commit message. Usage: .github/release_notes.sh v1.6.7 > notes.md
set -e
TAG="${1:?usage: release_notes.sh <tag>}"
HERE="$(cd "$(dirname "$0")" && pwd)"

cat "$HERE/release-template.md"

# %(contents) is the tag message for an annotated tag and the commit message for a lightweight one
MSG="$(git for-each-ref --format='%(contents)' "refs/tags/$TAG")"
[ -n "$MSG" ] || MSG="$(git log -1 --format=%B "$TAG")"
FIRST="$(printf '%s\n' "$MSG" | sed -n '1p' | sed -E 's/^v?[0-9]+(\.[0-9]+)*[^:]*: *//')"
FIRST="$(printf '%s' "${FIRST:0:1}" | tr '[:lower:]' '[:upper:]')${FIRST:1}"
REST="$(printf '%s\n' "$MSG" | sed '1d')"

echo
echo "## What's new in $TAG"
echo "- $FIRST"
# a longer annotation: keep the remaining lines as written (bullets etc.)
if [ -n "$(printf '%s' "$REST" | tr -d '[:space:]')" ]; then
  printf '%s\n' "$REST"
fi
echo "- Factory and update files for all four builds are attached."
