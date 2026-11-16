#!/bin/bash
# Rebuilds Resources/Icon/AppIcon.icns (release) and AppIcon-Dev.icns (development builds:
# same icon with a yellow "DEV" band) from the two SVGs. Run after editing them.
# StageRack-small.svg is used up to 64 px, StageRack.svg above.
# Rendering uses NSImage (scripts/render-svg.swift): it keeps the transparent macOS
# margin. JUCE's ICON_BIG crops the drawable to its bounds (full-bleed icon) and
# qlmanage paints a white background, so neither is usable.
set -euo pipefail
cd "$(dirname "$0")/../Resources/Icon"

tmp=$(mktemp -d)

# "DEV" band across the bottom of the rounded square (120-unit icon coordinates; the
# band follows the 27-unit corner radius). Letters are strokes: NSImage's SVG has no text.
band() { # stroke width
    cat <<SVG
    <path d="M0.02,94 H119.98 A27,27 0 0 1 93,120 H27 A27,27 0 0 1 0.02,94 Z" fill="#ffd60a"/>
    <path d="M41,100.5 V115.5 H45 Q51.5,115.5 51.5,108 Q51.5,100.5 45,100.5 Z M66,100.5 H56 V115.5 H66 M56,108 H63.5 M69.5,100.5 L75,115.5 L80.5,100.5"
          fill="none" stroke="#1b1e25" stroke-width="$1" stroke-linecap="round" stroke-linejoin="round"/>
SVG
}

devsvg() { # in.svg stroke out.svg
    band "$2" > "$tmp/band"
    awk -v bf="$tmp/band" '/<\/g>/ { while ((getline l < bf) > 0) print l; close(bf) } { print }' "$1" > "$3"
}

devsvg StageRack.svg       3.2 "$tmp/StageRack-Dev.svg"
devsvg StageRack-small.svg 4.5 "$tmp/StageRack-Dev-small.svg"

render() { # svg size outname
    swift ../../scripts/render-svg.swift "$1" "$2" "$set/$3"
}

build() { # small.svg big.svg out.icns
    set=$tmp/$(basename "$3" .icns).iconset
    mkdir "$set"
    render "$1" 16   icon_16x16.png
    render "$1" 32   icon_16x16@2x.png
    render "$1" 32   icon_32x32.png
    render "$1" 64   icon_32x32@2x.png
    render "$2" 128  icon_128x128.png
    render "$2" 256  icon_128x128@2x.png
    render "$2" 256  icon_256x256.png
    render "$2" 512  icon_256x256@2x.png
    render "$2" 512  icon_512x512.png
    render "$2" 1024 icon_512x512@2x.png
    iconutil -c icns -o "$3" "$set"
}

build StageRack-small.svg StageRack.svg AppIcon.icns
cp "$set/icon_512x512@2x.png" StageRack-1024.png
build "$tmp/StageRack-Dev-small.svg" "$tmp/StageRack-Dev.svg" AppIcon-Dev.icns
cp "$set/icon_512x512@2x.png" StageRack-Dev-1024.png
cp "$set/icon_32x32.png" "${ICON_PREVIEW_DIR:-$tmp}/dev-32.png" 2>/dev/null || true
rm -rf "$tmp"
echo "Resources/Icon/AppIcon.icns and AppIcon-Dev.icns updated"
