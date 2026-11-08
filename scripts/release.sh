#!/bin/bash
# Publishes the current version on GitHub, so the apps already installed offer it as an update.
#   1. set the version in CMakeLists.txt (project VERSION + STAGERACK_VERSION_LABEL)
#   2. commit
#   3. scripts/release.sh [notes.md]
# Builds, tests, packages, tags v<version> and creates the GitHub release with the zip.
# The tag MUST match the app's version: the updater compares them, and checks that the
# downloaded app says the same version.
set -euo pipefail
cd "$(dirname "$0")/.."

NOTES="${1:-}"
REPO="Bio00/StageRack"

if [ -n "$(git status --porcelain)" ]; then echo "Commit your changes first."; exit 1; fi

scripts/package.sh
EXE=dist/StageRack.app/Contents/MacOS/StageRack
$EXE --selftest | tail -1
$EXE --edit-selftest | tail -1

VERSION=$(sed -n 's/^project(StageRack VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
LABEL=$(sed -n 's/^set(STAGERACK_VERSION_LABEL "\(.*\)")/\1/p' CMakeLists.txt)
FULL="$VERSION${LABEL:+-$LABEL}"
TAG="v$FULL"
ZIP="dist/StageRack-$VERSION-macOS11-universal.zip"

if [ "$($EXE --check-update | head -1)" != "current $FULL" ]; then echo "Built app does not report $FULL"; exit 1; fi

git tag -a "$TAG" -m "StageRack $FULL"
git push origin HEAD "$TAG"

PRE=""; [ -n "$LABEL" ] && PRE="--prerelease"
if [ -n "$NOTES" ]; then
    gh release create "$TAG" "$ZIP" --repo "$REPO" $PRE --title "StageRack $FULL" --notes-file "$NOTES"
else
    gh release create "$TAG" "$ZIP" --repo "$REPO" $PRE --title "StageRack $FULL" --generate-notes
fi
echo "Released $TAG"
