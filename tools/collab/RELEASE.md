# Releasing Ploq (Windows, macOS, Linux)

One version number for all three; one signed update feed lists all three files. Batch fixes into a
release rather than shipping each one (a release means a full GitHub build, Apple notarization and an
update prompt for every user).

## 0. Before you start

- `src/collab/PLOQ_VERSION` bumped and committed on `main` of ploq-app-dev.
- Tests green: `tools\collab\qa-auto\battery.ps1` (everything `SYNC: IDENTICAL` except the known
  `struct` with the local relay) and `tools\collab\qa-auto\net.ps1 -Only inject,fuzz`.

## 1. All three platforms: built on GitHub Actions, from the public code

1. `node tools/collab/publish-public.mjs -m "Ploq x.y.z: ..." --push` publishes the source to
   specialith/ploq-app. That push runs the **Ploq release** workflow as a test build of Windows, macOS
   and Linux (no signing, no release).
2. Wait for it to pass (Actions tab, about 2–3 hours; the universal Mac build is the slowest).
3. Tag that exact commit: `git fetch public && git push public <public/main sha>:refs/tags/vx.y.z`.
   The tagged run repeats the builds. **The macos-sign job then waits for you**: open the run on GitHub
   and approve the `mac-signing` environment (Review deployments). It signs with the Developer ID and
   notarizes; the release job then attests every file and creates the GitHub Release with
   `Ploq-x.y.z-Setup.exe`, `Ploq-x.y.z.msi`, `Ploq-x.y.z.dmg`, `Ploq-x.y.z-x86_64.AppImage`.
4. Get and check them:
   ```
   gh release download vx.y.z -R specialith/ploq-app
   sha256sum -c SHA256SUMS.txt
   for f in Ploq-x.y.z*; do gh attestation verify "$f" -R specialith/ploq-app; done
   ```

The Mac signing secrets (Developer ID certificate as a .p12, its password, the App Store Connect API key
and its ID and issuer) live only in the `mac-signing` environment of specialith/ploq-app: required
reviewer specialith, usable from `main` and `v*` tags only. To replace the certificate, rebuild the .p12
on the MacBook from `~/.ploq/devid.key` + `devid.pem` + Apple's intermediate and update `MAC_CERT_P12` and
`MAC_CERT_PASSWORD` there (`gh secret set ... --env mac-signing`).

## 2. Fallback: building on the MacBook or in WSL

If GitHub can't build, the same scripts work locally (no attestation then).

macOS (`ssh ploq-mac`):
```
cd ~/ploq && git fetch origin && git checkout -B main origin/main
ARCHS="arm64;x86_64" bash tools/collab/macos/build.sh      # universal; starts from a clean Ploq.app
bash tools/collab/macos/package.sh                          # macdeployqt, sign, notarize, staple -> applebuild/Ploq-x.y.z.dmg
```
package.sh refuses to continue if anything in the app still links to the build machine's files, and
ends with `spctl ... source=Notarized Developer ID` for both the app and the .dmg.

Linux (WSL distro `PloqBuild`):
```
cd /root/ploq && git checkout --detach && git fetch -f /mnt/c/ms main:release-main && git checkout release-main
nice -n 19 bash tools/collab/linux/build_appimage.sh        # -> build-appimage/Ploq-x.y.z-x86_64.AppImage
```
(From Git Bash on Windows, prefix `wsl` commands that use /mnt/c paths with `MSYS_NO_PATHCONV=1`.)
Smoke test: `MU_QT_QPA_PLATFORM=offscreen ./Ploq-x.y.z-x86_64.AppImage --appimage-extract-and-run` stays up.

## 3. Windows setup app built locally (only if a release lacks it)

`powershell -File tools\collab\installer\setup\build_setup.ps1 -Msi Ploq-x.y.z.msi`

## 4. One signed feed, then upload

On the PC (the update-signing key never leaves `%USERPROFILE%\.ploq\update-signing-key.pem`):
```
node tools/collab/installer/make_release.mjs Ploq-x.y.z.msi Ploq-x.y.z-Setup.exe Ploq-x.y.z.dmg Ploq-x.y.z-x86_64.AppImage --notes "Ploq x.y.z: ..."
```
Upload from `C:\ploq-server` (wrangler), the **installers first, latest.json last**:
```
npx wrangler r2 object put collab-scores/releases/Ploq-x.y.z.msi --file <msi> --content-type application/x-msi --remote
npx wrangler r2 object put collab-scores/releases/Ploq-x.y.z-Setup.exe --file <exe> --content-type application/vnd.microsoft.portable-executable --remote
npx wrangler r2 object put collab-scores/releases/Ploq-x.y.z.dmg --file <dmg> --content-type application/x-apple-diskimage --remote
npx wrangler r2 object put collab-scores/releases/Ploq-x.y.z-x86_64.AppImage --file <AppImage> --content-type application/vnd.appimage --remote
npx wrangler r2 object put collab-scores/releases/latest.json --file <latest.json> --content-type application/json --remote
```
Check each file downloads from `https://collab.ploq.app/updates/files/<name>` with the SHA-256 in the feed.

## 5. Website and server

- ploq.app/download offers each platform's file as soon as the signed feed lists it (it checks the feed's
  signature itself). On Windows it offers the Setup.exe when the feed lists one, else the .msi.
  Site copy that names platforms (home button, JSON-LD, llms.txt): ploq-web.
- Public server code: `node scripts/publish-public.mjs -m "..." --push` in C:\ploq-server.
- Keep the debug symbols: Windows from the workflow's `windows-symbols` artifact, macOS dSYMs are not
  kept yet, the Linux AppImage is stripped.
