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

## Code signing (Azure Artifact Signing)

Unsigned, Windows SmartScreen warns ("Windows protected your PC" > More info > Run anyway).
Ploq signs with **Azure Artifact Signing** (formerly Trusted Signing): Microsoft verifies
Ploq, LLC once, then issues a fresh 3-day certificate for every signing, timestamped by
Microsoft so signatures stay valid after it expires. `build_installer.ps1` signs through
`sign.ps1`, and only files that aren't already signed (Qt and the runtime ship signed by
their makers), so a release costs 2 signatures: `Ploq.exe` and the `.msi`.

Eligibility (Microsoft Learn, 2026-11): organizations in the US (and 11 other places) or
individuals in the US/Canada; no minimum company age is stated; a **paid** Azure
subscription (not free/trial). Identity validation takes 1 to 30 business days and is done
in the Azure portal. SmartScreen reputation still builds up with downloads after signing.

### One-time setup

1. **Azure**: an Azure account with a pay-as-you-go subscription (portal.azure.com).
2. **Artifact Signing account**: in the portal, create an *Artifact Signing account*
   (Basic plan), e.g. name `ploq-signing`, region *East US*. Note the region: it decides the
   endpoint (East US = `https://eus.codesigning.azure.net`; others in `sign.ps1`'s sources
   on Microsoft Learn, "how-to-signing-integrations").
3. **Roles**: on that account, give yourself *Artifact Signing Identity Verifier* (to request
   validation) and *Artifact Signing Certificate Profile Signer* (to sign).
4. **Identity validation** (account > Identity validations > New > Public): legal name
   "Ploq, LLC", the business identifier (EIN), address, website `https://ploq.app`, a primary
   email at `@ploq.app` and a secondary email. Microsoft may ask for the articles of
   organization (issued within 12 months) and a domain invoice.
5. **Certificate profile**, once validation is approved: account > Certificate profiles >
   Create > *Public Trust*, pick the validation, e.g. name `ploq`.
6. **This PC**: install the Azure CLI (`winget install -e --id Microsoft.AzureCLI`), then
   `az login`. Create `%USERPROFILE%\.ploq\artifact-signing.json`:

   ```json
   {
     "Endpoint": "https://eus.codesigning.azure.net",
     "CodeSigningAccountName": "ploq-signing",
     "CertificateProfileName": "ploq",
     "ExcludeCredentials": ["ManagedIdentityCredential", "WorkloadIdentityCredential",
       "EnvironmentCredential", "SharedTokenCacheCredential", "VisualStudioCredential",
       "VisualStudioCodeCredential", "AzurePowerShellCredential", "AzureDeveloperCliCredential",
       "InteractiveBrowserCredential"]
   }
   ```

   (`ExcludeCredentials` leaves only the Azure CLI sign-in, so it never tries anything else.)

That's all: the next `build_installer.ps1` signs. `sign.ps1` fetches Microsoft's signing client
(`Microsoft.ArtifactSigning.Client` **1.0.146**, the newest on .NET 8; 1.0.151+ needs the .NET 10
runtime, without which signtool crashes silently) into `%USERPROFILE%\tools\artifact-signing`,
checks it's signed by Microsoft, signs with SignTool 10.0.22621.755+, and verifies every
signature. A certificate in the Windows store still works instead: `PLOQ_SIGN_CERT_THUMBPRINT`.

Once releases are signed, make the updater insist on it: define `PLOQ_REQUIRE_AUTHENTICODE`
in `src/collab/CMakeLists.txt` (`verifyAuthenticode` in `ploqupdate.cpp`). Only once signing works:
a build with it refuses every update that isn't signed.

## Testing locally

Run ploq-server with `npx wrangler dev --port 8790 --ip 127.0.0.1 --local-upstream 127.0.0.1:8790
--persist-to <dir>`, upload with `--local --persist-to <dir>` instead of `--remote`, and start
the installed Ploq with `PLOQ_UPDATE_URL=http://127.0.0.1:8790/updates/latest.json` (https, or
http to this computer only; signatures are checked either way) and
`COLLAB_SERVER=http://127.0.0.1:8790`.
