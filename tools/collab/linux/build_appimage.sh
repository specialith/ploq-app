#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Build Ploq-<version>-x86_64.AppImage (release mode: settings in the "Ploq" folders).
# Reuses upstream MuseScore's Linux CI: buildscripts/ci/linux/setup.sh (dependencies,
# g++-10, PipeWire) and buildscripts/ci/linux/tools/make_appimage.sh (linuxdeploy,
# appimagetool). Build on Ubuntu 22.04 like upstream, so the AppImage runs on
# distributions from 2022 on (glibc 2.35).
#
#   tools/collab/linux/build_appimage.sh --setup   # once: apt packages (root or sudo), Qt 6.10.2 in ~/Qt
#   tools/collab/linux/build_appimage.sh           # configure, build, package
#
# Options: --setup, -j <jobs> (default: half the cores), --build-dir <dir> (default: build-appimage).
# Output: <build dir>/Ploq-<version>-x86_64.AppImage. The build runs at low priority (nice).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SETUP=false
JOBS=$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))
BUILD_DIR="${ROOT}/build-appimage"
QT_VERSION=6.10.2

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --setup) SETUP=true ;;
        -j|--jobs) JOBS="$2"; shift ;;
        --build-dir) BUILD_DIR="$2"; shift ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
    shift
done

export QT_ROOT_DIR="${QT_ROOT_DIR:-$HOME/Qt/${QT_VERSION}/gcc_64}"
cd "${ROOT}"

if $SETUP; then
    SUDO=""; [[ "$(id -u)" == 0 ]] || SUDO=sudo
    export DEBIAN_FRONTEND=noninteractive
    $SUDO apt-get update
    # cmake/ninja: upstream CI has them preinstalled; pip gets current ones.
    $SUDO apt-get install -y --no-install-recommends sudo python3-pip ccache file libfuse2 libsecret-1-dev libgcrypt20-dev
    # Preinstalled on GitHub's runners, needed by Qt's xcb/wayland plugins when linuxdeploy bundles them.
    $SUDO apt-get install -y --no-install-recommends libxcb-cursor0 libxcb-shape0 libxcb-xfixes0 libxcb-sync1 libxcb-shm0 \
        libxcb-util1 libxcb-xinput0 libwayland-cursor0 libwayland-egl1 libxcb-glx0 libxcb-present0 libxcb-dri3-0
    python3 -m pip install --user "cmake>=3.26,<4" ninja aqtinstall
    export PATH="$HOME/.local/bin:$PATH"
    bash buildscripts/ci/linux/setup.sh --arch x86_64 --build-pipewire
    if [[ ! -x "${QT_ROOT_DIR}/bin/qmake" ]]; then
        python3 -m aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 -O "$HOME/Qt" \
            -m qt5compat qtnetworkauth qtshadertools qtwebsockets
    fi
fi

export PATH="$HOME/.local/bin:${QT_ROOT_DIR}/bin:$PATH"
set +u # environment.sh expands variables that may be unset
source "$HOME/build_tools/environment.sh" # CC/CXX (g++-10) and PipeWire, from setup.sh
set -u

VERSION="$(head -n1 src/collab/PLOQ_VERSION | tr -d '[:space:]')"
NAME="Ploq-${VERSION}-x86_64"
APPDIR="${BUILD_DIR}/${NAME}.AppDir"
REVISION="$(git rev-parse --short=7 HEAD)"
echo "Ploq ${VERSION} (${REVISION}): ${JOBS} jobs, build in ${BUILD_DIR}"

mkdir -p "${BUILD_DIR}"
LAUNCHER=()
command -v ccache >/dev/null && LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
nice -n 19 cmake -S . -B "${BUILD_DIR}" -GNinja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_PREFIX_PATH="${QT_ROOT_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${APPDIR}" \
    -DMUSE_APP_BUILD_MODE=release \
    -DMUSESCORE_REVISION="${REVISION}" \
    -DMUSE_ENABLE_UNIT_TESTS=OFF \
    -DMUSE_MODULE_AUDIO_PIPEWIRE=ON \
    -DMUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT=OFF \
    -DCMAKE_SKIP_RPATH=ON \
    -DPLOQ_APPIMAGE=ON \
    "${LAUNCHER[@]}"
nice -n 19 cmake --build "${BUILD_DIR}" -j "${JOBS}"

rm -rf "${APPDIR}"
nice -n 19 cmake --build "${BUILD_DIR}" --target install/strip > "${BUILD_DIR}/install.log"

# AppDir root: what appimagetool and linuxdeploy look for.
cd "${APPDIR}"
ln -sf . usr # installed into the AppDir root, but some tools expect usr/
cp share/applications/app.ploq.Ploq.desktop .
cp share/icons/hicolor/256x256/apps/app.ploq.Ploq.png .
cd "${BUILD_DIR}"

# make_appimage.sh: linuxdeploy (+ Qt plugin) bundles the libraries, appimagetool packs.
export LD_LIBRARY_PATH="${QT_ROOT_DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QMAKE="${QT_ROOT_DIR}/bin/qmake"
export APPIMAGE_EXTRACT_AND_RUN=1 # the tools are AppImages themselves: no FUSE needed (containers, WSL)
unset UPDATE_INFORMATION          # Ploq on Linux never updates itself (it opens ploq.app/download)
rm -f "${NAME}.AppImage"
nice -n 19 bash "${ROOT}/buildscripts/ci/linux/tools/make_appimage.sh" "${APPDIR}" "${NAME}.AppImage" x86_64 --build-pipewire

ls -l "${BUILD_DIR}/${NAME}.AppImage"
echo "Done: ${BUILD_DIR}/${NAME}.AppImage"
