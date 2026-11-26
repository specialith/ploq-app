# Ploq

**Real-time collaboration for MuseScore.** Open a score in Ploq, press **Share**, and send the
link. Everyone in the score sees each other's edits, cursors and selections live.

**[Download Ploq for Windows](https://ploq.app/download)** · [ploq.app](https://ploq.app)

Ploq is a fork of [MuseScore Studio](https://github.com/musescore/MuseScore) 4.7.7 with a
collaboration module added. It is not affiliated with or endorsed by Muse Group.

## What's here

- `src/collab/`: the collaboration module (sharing, sign-in, live sync, updates). Upstream
  MuseScore files are only touched by small hooks marked `COLLAB:`.
- `docs/collab/`: how it works: `ARCHITECTURE_NOTES.md`, the sync `PROTOCOL.md`, and
  `UPSTREAM.md` (what changed in MuseScore's own files).
- `tools/collab/`: the Windows build wrapper, the installer and update tools.

The server Ploq connects to is **[ploq-server](https://github.com/specialith/ploq-server)**, a
Cloudflare Worker. ploq.app runs one for everyone, and you can run your own.

## Build it (Windows)

You need Visual Studio 2022 with C++ desktop development, [Qt 6.10.2](https://www.qt.io/download-qt-installer-oss)
(MSVC 2022 64-bit), CMake and Ninja. Then create `build_overrides.cmake` in the repo root (it's
ignored by git) with your Qt path:

```cmake
set(ENV{QTDIR} "C:/Qt/6.10.2/msvc2022_64")
list(APPEND CONFIGURE_ARGS -G Ninja)
```

and build:

```
tools\collab\build.bat        :: configure, build and install
tools\collab\build.bat run    :: start the built app
```

For a release build and the installer, see `tools/collab/installer/README.md`. Other platforms
build the same way as MuseScore Studio; see MuseScore's
[developer documentation](https://github.com/musescore/MuseScore/wiki).

## Use your own server

Ploq uses ploq.app's server unless the `COLLAB_SERVER` environment variable says otherwise:

```
setx COLLAB_SERVER "https://ploq-server.your-name.workers.dev"
```

then restart Ploq. Development builds stay offline unless `COLLAB_SERVER` is set.

## Contributing

Bug reports and pull requests are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md). Please report
security problems privately through **Security > Report a vulnerability** on this repository,
not in a public issue.

## License

Ploq is free software under the GNU General Public License version 3, like MuseScore Studio; see
[LICENSE.txt](LICENSE.txt). Third-party components keep their own licenses (see `thirdparty/`
and `tools/collab/installer/THIRD_PARTY_NOTICES.txt`).

MuseScore and Muse Sounds are trademarks of Muse Group. Muse Sounds are not part of Ploq.
