#!/bin/bash
# Ploq for macOS: Ploq.app (from tools/collab/macos/build.sh) -> signed, notarized Ploq-<version>.dmg
#
#   tools/collab/macos/package.sh                 # sign + notarize (needs the settings below)
#   tools/collab/macos/package.sh --no-notarize   # sign only (quick local test)
#   tools/collab/macos/package.sh --no-sign       # unsigned .dmg (anyone can build this)
#   tools/collab/macos/package.sh --no-sign --no-dmg  # only bundle Qt into Ploq.app and check it (CI build job)
#
# Signing and notarization settings live outside the repo, in ~/.ploq/notary.env:
#   SIGN_IDENTITY="Developer ID Application: Ploq, LLC (5NRAWKP4N6)"
#   NOTARY_KEY=/path/to/AuthKey_XXXXXXXXXX.p8   NOTARY_KEY_ID=XXXXXXXXXX   NOTARY_ISSUER=<uuid>
#   NOTARY_TEAM_ID=5NRAWKP4N6
# and, if the signing identity is in its own keychain, ~/.ploq/keychain-pass unlocks
# ~/Library/Keychains/ploq-signing.keychain-db.
#
# Steps follow upstream's buildscripts/packaging/macOS/package.sh: macdeployqt, hardened-runtime
# signing with entitlements, notarize + staple the app, then a signed, notarized, stapled .dmg.
set -euo pipefail

DO_SIGN=1
DO_NOTARIZE=1
DO_DMG=1
for a in "$@"; do
    case "$a" in
        --no-sign) DO_SIGN=0; DO_NOTARIZE=0 ;;
        --no-notarize) DO_NOTARIZE=0 ;;
        --no-dmg) DO_DMG=0 ;;
        *) echo "unknown option $a"; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
APP="applebuild/Ploq.app"
VERSION="$(head -1 src/collab/PLOQ_VERSION | tr -d '[:space:]')"
OUT="applebuild/Ploq-${VERSION}.dmg"
ENTITLEMENTS="buildscripts/packaging/macOS/entitlements.plist"
QT_BIN="${QT_ROOT_DIR:-$HOME/Qt/6.10.2/macos}/bin"

[ -d "$APP" ] || { echo "No $APP: build first (tools/collab/macos/build.sh)"; exit 1; }

if [ "$DO_SIGN" = 1 ]; then
    # shellcheck disable=SC1090
    source "$HOME/.ploq/notary.env"
    # shellcheck disable=SC1091
    source "$ROOT/tools/collab/macos/signing.sh" # the Developer ID keychain, for this script only
fi

echo "== macdeployqt (Qt frameworks and plugins into the app)"
if [ ! -d "$APP/Contents/Frameworks/QtCore.framework" ]; then
    "$QT_BIN/macdeployqt" "$APP" -qmldir=. -verbose=1
    find "$APP/Contents" -type d -name "*.dSYM" -exec rm -r {} +
    # As upstream: Resources/qml -> qml_mu (Qt's deployed QML modules live under Resources/qml).
    if [ -d "$APP/Contents/Resources/qml" ] && [ ! -d "$APP/Contents/Resources/qml_mu" ]; then
        mv "$APP/Contents/Resources/qml" "$APP/Contents/Resources/qml_mu"
        sed -i '' 's:Resources/qml:Resources/qml_mu:g' "$APP/Contents/Resources/qt.conf"
    fi
fi

# Nothing in the app may load from this machine (e.g. ~/Qt or the deps folder): that works here and fails on
# every other Mac.
leaks=$(find "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" -type f \( -perm +111 -o -name "*.dylib" \) \
    -exec otool -L {} \; | grep -E "^[[:space:]]+(/Users/|$HOME)" | sort -u || true)
if [ -n "$leaks" ]; then
    echo "The app still links to files on this machine:"; echo "$leaks"; exit 1
fi
echo "== no links to this machine's files"

notarize() { # $1 = file to submit (.zip or .dmg)
    xcrun notarytool submit "$1" --key "$NOTARY_KEY" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER" \
        --wait --timeout 60m --output-format json > "$1.notary.json"
    cat "$1.notary.json"; echo
    if ! grep -q '"status" *: *"Accepted"' "$1.notary.json"; then
        id=$(sed -n 's/.*"id" *: *"\([^"]*\)".*/\1/p' "$1.notary.json" | head -1)
        [ -n "$id" ] && xcrun notarytool log "$id" --key "$NOTARY_KEY" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER"
        echo "Notarization was not accepted"; exit 1
    fi
}

if [ "$DO_SIGN" = 1 ]; then
    echo "== sign (Developer ID, hardened runtime, secure timestamp)"
    # Inside out: every nested binary first, then the app itself with its entitlements.
    find "$APP/Contents" \( -name "*.dylib" -o -name "*.so" \) -type f -print0 |
        xargs -0 -n 20 codesign --force --options runtime --timestamp -s "$SIGN_IDENTITY"
    for fw in "$APP"/Contents/Frameworks/*.framework; do
        [ -d "$fw" ] && codesign --force --options runtime --timestamp -s "$SIGN_IDENTITY" "$fw"
    done
    find "$APP/Contents/MacOS" -type f ! -name Ploq -perm +111 -print0 |
        xargs -0 -I{} codesign --force --options runtime --timestamp -s "$SIGN_IDENTITY" {}
    codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" -s "$SIGN_IDENTITY" "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"

    if [ "$DO_NOTARIZE" = 1 ]; then
        echo "== notarize the app"
        ZIP="applebuild/Ploq-notarize.zip"
        rm -f "$ZIP"
        ditto -c -k --keepParent "$APP" "$ZIP"
        notarize "$ZIP"
        rm -f "$ZIP" "$ZIP.notary.json"
        xcrun stapler staple "$APP"
        xcrun stapler validate "$APP"
        spctl --assess --type execute -vv "$APP"
    fi
fi

[ "$DO_DMG" = 1 ] || { echo "== $APP ready (no .dmg asked for)"; exit 0; }

echo "== dmg"
STAGE="applebuild/dmg-stage"
rm -rf "$STAGE" "$OUT"
mkdir -p "$STAGE"
ditto "$APP" "$STAGE/Ploq.app"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "Ploq" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 -ov "$OUT"
rm -rf "$STAGE"

if [ "$DO_SIGN" = 1 ]; then
    codesign --force --timestamp -s "$SIGN_IDENTITY" "$OUT"
    if [ "$DO_NOTARIZE" = 1 ]; then
        echo "== notarize the dmg"
        notarize "$OUT"
        rm -f "$OUT.notary.json"
        xcrun stapler staple "$OUT"
        xcrun stapler validate "$OUT"
        spctl --assess --type open --context context:primary-signature -vv "$OUT"
    fi
fi

shasum -a 256 "$OUT"
ls -la "$OUT"
