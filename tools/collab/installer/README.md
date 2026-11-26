# Ploq Windows installer and auto-update

One file, `Ploq-<version>.msi`, installs Ploq for the current Windows user (no admin
prompt), adds a Start-menu shortcut and an uninstaller (Settings > Apps > Ploq), and
registers `ploq://` links. Ploq then keeps itself up to date from Ploq's server, and it
only ever installs updates that are signed by you.

## What the installer does (and doesn't)

- Installs to `%LOCALAPPDATA%\Programs\Ploq` (`bin\Ploq.exe`, `Open-Link.ps1`, `LICENSE.txt`,
  `THIRD_PARTY_NOTICES.txt`, and `Inter-LICENSE.txt` for the name-tag font built into Ploq.exe).
- `ploq://` links run `powershell -File <install>\Open-Link.ps1 "<link>"`. That script treats a
  link strictly as text and accepts only exact link shapes (security issue 1). It never goes
  through cmd.exe or a .bat file. The old `collabscore://` handler is removed.
- **Does not** register `.mscz` or any other file type, and doesn't touch any MuseScore file,
  folder, registry key or the `musescore://` scheme. The `UpgradeCode` in `Ploq.wxs` is Ploq's own.
- Uninstall removes the program, shortcut and `ploq://` handler. Like most apps, it keeps the
  user's settings (`%APPDATA%\Ploq\Ploq.ini`, `%LOCALAPPDATA%\Ploq\Ploq`) and scores.

## Building an installer

Needs WiX Toolset 3.14 binaries in `%USERPROFILE%\tools\wix314` (download `wix314-binaries.zip`
from github.com/wixtoolset/wix3/releases and unzip; no admin needed), and a **release-mode**
build with Qt deployed next to the exe. In `build_overrides.cmake`:

    list(APPEND CONFIGURE_ARGS -DMUSE_APP_BUILD_MODE=release -DMUE_RUN_WINDEPLOYQT=ON)

Then:

    tools\collab\build.bat
    powershell -ExecutionPolicy Bypass -File tools\collab\installer\build_installer.ps1

Output: `builds\installer\Ploq-<version>.msi`. The version comes from `src\collab\PLOQ_VERSION`
(Ploq's own number; MuseScore's 4.7.7 stays inside for score files).

## Publishing an update

1. Bump `src\collab\PLOQ_VERSION` (e.g. `1.0.1`), rebuild, build the installer.
2. Sign the update description (needs the release key, see below):

       node tools\collab\installer\make_release.mjs builds\installer\Ploq-1.0.1.msi --notes "What's new"

   This writes `builds\installer\release\latest.json` + a copy of the `.msi`.
3. Upload both to the server's storage (ploq-server repo, Cloudflare R2 bucket `collab-scores`):

       npx wrangler r2 object put collab-scores/releases/Ploq-1.0.1.msi --file <msi> --content-type application/x-msi --remote
       npx wrangler r2 object put collab-scores/releases/latest.json --file <latest.json> --content-type application/json --remote

   Upload the `.msi` **first**, then `latest.json`. The server (`/updates/latest.json`,
   `/updates/files/Ploq-x.y.z.msi`) needs the ploq-server version with the `/updates/` route.
4. Every Ploq checks on startup and shows "A new version of Ploq is available!". **Install
   update** downloads the file and checks it. **Close** quits Ploq, and Windows Installer runs
   with a progress bar only. Ploq reopens on the new version.

## How updates are checked (why an attacker can't push one)

- `latest.json` must carry a valid ECDSA P-256 signature from the **release key**. The public
  half is built into Ploq (`src/collab/ploqupdate.cpp`). Unsigned or altered: ignored.
- The downloaded installer must match the size and SHA-256 in that signed text, both when
  downloaded and again right before it runs. Otherwise it's thrown away and the user sees
  "Couldn't update Ploq".
- Only a newer Ploq version is offered, and only a file named `Ploq-x.y.z.msi` is accepted.
- Tested 2026-11-24: altered `latest.json` -> rejected; altered installer -> rejected after
  download; genuine 1.0.0 -> 1.0.1 update installed and Ploq reopened as 1.0.1.

## The release key: keep it safe

- Private key: `%USERPROFILE%\.ploq\update-signing-key.pem` on the machine that made it.
  It is **not** in any repo. **Back it up** somewhere offline, e.g. an encrypted USB stick or a
  password manager. Without it you can't publish updates to installed copies. Anyone who
  has it can push updates to every Ploq user.
- To change keys: ship one update signed with the OLD key whose app contains the NEW public key
  (`RELEASE_KEY_X/Y` in `ploqupdate.cpp`); sign everything after that with the new key.

## Code signing (when the certificate arrives)

Right now the installer and Ploq.exe are unsigned, so Windows SmartScreen will warn ("Windows
protected your PC" > More info > Run anyway) until the files are signed and have built
reputation. To turn signing on:

1. Install the certificate (or plug in its USB token).
2. Set `PLOQ_SIGN_CERT_THUMBPRINT` to its thumbprint (see `sign.ps1`). `build_installer.ps1`
   then signs Ploq.exe, its DLLs and the `.msi`, with a timestamp.
3. Optionally make the updater insist on it: define `PLOQ_REQUIRE_AUTHENTICODE` in
   `src/collab/CMakeLists.txt` (`verifyAuthenticode` in `ploqupdate.cpp`).

## Testing locally

Run ploq-server with `npx wrangler dev --port 8790 --ip 127.0.0.1 --local-upstream 127.0.0.1:8790
--persist-to <dir>`, upload with `--local --persist-to <dir>` instead of `--remote`, and start
the installed Ploq with `PLOQ_UPDATE_URL=http://127.0.0.1:8790/updates/latest.json` (https, or
http to this computer only; signatures are checked either way) and
`COLLAB_SERVER=http://127.0.0.1:8790`.
