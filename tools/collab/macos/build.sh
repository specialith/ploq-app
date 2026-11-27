#!/bin/bash
# Ploq for macOS: build + install Ploq.app into applebuild/ (release mode), like upstream's
# buildscripts/ci/macos/build.sh. Then tools/collab/macos/package.sh makes the signed .dmg.
#
#   tools/collab/macos/build.sh                     # Apple Silicon only (quick test builds)
#   ARCHS="arm64;x86_64" tools/collab/macos/build.sh  # universal: Intel + Apple Silicon (releases)
#   JOBS=8 tools/collab/macos/build.sh              # parallel compile jobs (default 6)
#
# Needs: Xcode Command Line Tools, CMake + Ninja (e.g. `python3 -m pip install --user cmake ninja`),
# MuseScore's universal Qt 6.10.2 in ~/Qt/6.10.2/macos and its macOS dependencies in
# ~/musescore_deps_macos (see buildscripts/ci/macos/install_qt.sh and download_dependencies.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
QT="${QT_ROOT_DIR:-$HOME/Qt/6.10.2/macos}"
export PATH="$HOME/Library/Python/3.9/bin:$QT/bin:$PATH"
export QT_ROOT_DIR="$QT" QT_PLUGIN_PATH="$QT/plugins" QML2_IMPORT_PATH="$QT/qml"

# Start from a clean bundle: install only overwrites files, so a Ploq.app that package.sh already ran
# macdeployqt on would keep its bundled Qt, package.sh would skip macdeployqt, and the new binary would
# still point at ~/Qt (fine on this Mac, broken on everyone else's).
rm -rf "$ROOT/applebuild/Ploq.app"

MUSESCORE_MACOS_DEPS_PATH="${MUSESCORE_MACOS_DEPS_PATH:-$HOME/musescore_deps_macos}" \
CMAKE_OSX_ARCHITECTURES="${ARCHS:-arm64}" \
MUSESCORE_INSTALL_DIR="../applebuild" \
MUSE_APP_BUILD_MODE=release \
MUSESCORE_BUILD_NUMBER="${BUILD_NUMBER:-1}" \
MUSESCORE_REVISION="$(git rev-parse --short=7 HEAD)" \
MUSESCORE_BUILD_CRASHPAD_CLIENT=OFF \
MUSESCORE_BUILD_VST_MODULE=ON \
MUSESCORE_BUILD_WEBSOCKET=ON \
MUSESCORE_BUILD_UNIT_TESTS=OFF \
nice -n 15 bash ./ninja_build.sh -t installrelwithdebinfo -j "${JOBS:-6}"

# Sign the build with the Developer ID when it's set up (~/.ploq/notary.env, see package.sh), so macOS
# sees every rebuild as the same app: privacy answers ("access your Documents folder") and Keychain
# access then carry over between builds instead of being asked again. Not a release: package.sh
# re-signs everything with the hardened runtime and notarizes.
if [ -f "$HOME/.ploq/notary.env" ] && [ -d applebuild/Ploq.app ]; then
    # shellcheck disable=SC1090
    source "$HOME/.ploq/notary.env"
    # shellcheck disable=SC1091
    source "$ROOT/tools/collab/macos/signing.sh"
    codesign --force --deep -s "$SIGN_IDENTITY" applebuild/Ploq.app && echo "signed applebuild/Ploq.app for local testing"
fi
