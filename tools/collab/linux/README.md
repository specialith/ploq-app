# Ploq for Linux (AppImage)

`Ploq-<version>-x86_64.AppImage`: one file, runs on x86_64 distributions from 2022 on
(built on Ubuntu 22.04 like upstream MuseScore, glibc 2.35). Release mode: settings in
`~/.config/Ploq/Ploq.conf`, data in `~/.local/share/Ploq/Ploq`; a MuseScore install's files
are never touched.

## Building

On Ubuntu 22.04 (a WSL distro works: build in its own ext4 filesystem, not under `/mnt/c`):

```sh
git clone <ploq-app-dev> ~/ploq && cd ~/ploq
tools/collab/linux/build_appimage.sh --setup   # once: apt packages, g++-10, PipeWire, Qt 6.10.2 (aqtinstall) in ~/Qt
tools/collab/linux/build_appimage.sh           # -> build-appimage/Ploq-<version>-x86_64.AppImage
```

The script reuses upstream's CI scripts (`buildscripts/ci/linux/setup.sh`,
`buildscripts/ci/linux/tools/make_appimage.sh`: linuxdeploy + appimagetool), builds at low
priority with half the cores (`-j <n>` to change), and adds Ploq's own layout from
`PloqLinuxPackaging.cmake`: `bin/ploq`, `app.ploq.Ploq.desktop`, the Ploq icon, `AppRun`.
The version is `src/collab/PLOQ_VERSION`.

## What's different on Linux

| | Windows | Linux |
|---|---|---|
| `ploq://` links | registry (HKCU), `Ploq.exe --ploq-link "%1"` | desktop entry `x-scheme-handler/ploq`, `Exec="<AppImage>" %u`. The AppImage writes a hidden `~/.local/share/applications/app.ploq.PloqLinks.desktop` on start and makes it the default (`xdg-mime`), redone when the AppImage moves. Same exact link shapes (`src/collab/ploqlinux.cpp`) |
| Sign-in hand-over | named pipe `\\.\pipe\ploq-signin-<state>` | the same `QLocalServer`, a Unix socket `/tmp/ploq-signin-<state>` |
| Remembered sign-in | Credential Manager | the desktop keyring through libsecret (GNOME Keyring, KWallet, KeePassXC). No keyring: signed in until Ploq closes, never in a plain file |
| Updates | verified `.msi`, installed on close | the same signed feed; a newer version shows the dialog with **Go to download page** (`https://ploq.app/download`). Nothing is downloaded or installed |
| Crash reports | minidump in `%LOCALAPPDATA%\Ploq\crashes` | MuseScore's signal handler (one log line) |
| Listen in MuseScore Studio | registry / Program Files | `mscore4portable`, `musescore4`, `musescore`, `mscore` in PATH, the Flatpak `org.musescore.MuseScore`, `MuseScore*.AppImage` in `~/Applications`, `~/.local/bin`, `~/Downloads` (executable only). The copy goes to `~/.cache/Ploq/Ploq/` (a Flatpak can't see `/tmp`) |

## Testing without windows

Qt's offscreen platform. Ploq on Linux forces X11 unless `MU_QT_QPA_PLATFORM` is set, so set both:

```sh
unset DISPLAY WAYLAND_DISPLAY PULSE_SERVER   # under WSL: WSLg would show windows / play sound on Windows
QT_QPA_PLATFORM=offscreen MU_QT_QPA_PLATFORM=offscreen ./Ploq-1.0.6-x86_64.AppImage demos/Amazing_grace.mscz
```

Two-instance sync test through the local relay (development build, `COLLAB_AUTOBOT`):

```sh
tools/collab/linux/qa-pair.sh basic build.dev.install/bin/ploq   # RELAY_JS=<ploq-server>/relay/server.mjs NODE=<node 18+>
```
