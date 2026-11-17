#!/bin/bash
# Publishes the current version on GitHub, so the apps already installed offer it as an update.
#   1. set the version in CMakeLists.txt (project VERSION + STAGERACK_VERSION_LABEL)
#   2. commit
#   3. scripts/release.sh [notes.md]
# Without notes.md the release notes are the "## <version>" section of CHANGELOG.md (the same
# text the app shows in What's New) plus the install instructions; the section must exist.
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

if [ -z "$NOTES" ]; then
    NOTES="dist/release-notes-$FULL.md"
    awk -v v="$FULL" '
        /^## / { on = ($2 == v || $2 == "v" v); next }
        on' CHANGELOG.md | sed -e '/./,$!d' > "$NOTES"
    if [ ! -s "$NOTES" ]; then echo "CHANGELOG.md has no section '## $FULL': write the notes first."; exit 1; fi
    cat >> "$NOTES" <<EOT

<!-- install -->
### Installazione
- Da 0.2.0-beta in poi: StageRack propone l'aggiornamento all'avvio (o menu StageRack → Check for Updates...). L'audio si ferma per qualche secondo: non farlo durante uno spettacolo. Dopo l'aggiornamento le novità compaiono in una finestra (e restano nel menu StageRack → What's New...).
- Prima installazione: scarica \`$(basename "$ZIP")\`, sposta \`StageRack.app\` in Applicazioni, la prima volta tasto destro → **Apri** (firma ad-hoc, non Developer ID Apple).

È una **beta**: provala bene prima di usarla su un palco vero. Segnalazioni nelle Issues.

Licenza: GNU AGPLv3 (usa JUCE).
EOT
fi

git tag -a "$TAG" -m "StageRack $FULL"
git push origin HEAD "$TAG"

PRE=""; [ -n "$LABEL" ] && PRE="--prerelease"
gh release create "$TAG" "$ZIP" --repo "$REPO" $PRE --title "StageRack $FULL" --notes-file "$NOTES"
echo "Released $TAG"
