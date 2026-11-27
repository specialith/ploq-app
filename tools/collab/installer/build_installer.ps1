# Ploq - build the Windows installer: builds\installer\Ploq-<version>.msi
#
# Usage (after tools\collab\build.bat in a release-mode build, see README.md):
#   powershell -ExecutionPolicy Bypass -File tools\collab\installer\build_installer.ps1
#
# Steps: stage the installed build (+ Open-Link.ps1, licences) -> code-sign
# (only if a certificate is configured, see sign.ps1) -> WiX heat/candle/light
# -> sign the .msi. Version comes from src\collab\PLOQ_VERSION.

param(
    [string]$InstallDir = "",
    [string]$Wix = "$env:USERPROFILE\tools\wix314"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $InstallDir) { $InstallDir = "$root\builds\Win-Qt6.10.2-msvc2022_64-Ninja-RelWithDebInfo\install" }
$version = (Get-Content "$root\src\collab\PLOQ_VERSION" -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "PLOQ_VERSION must look like 1.2.3, got '$version'" }
$out = "$root\builds\installer"
$stage = "$out\stage"
$obj = "$out\obj"
$msi = "$out\Ploq-$version.msi"

foreach ($tool in "heat.exe", "candle.exe", "light.exe") {
    if (-not (Test-Path "$Wix\$tool")) { throw "WiX 3 not found at $Wix (see README.md)" }
}
if (-not (Test-Path "$InstallDir\bin\Ploq.exe")) { throw "Build first: tools\collab\build.bat ($InstallDir\bin\Ploq.exe missing)" }
if (-not (Test-Path "$InstallDir\bin\Qt6Core.dll")) { throw "Qt DLLs missing next to Ploq.exe: configure with -DMUE_RUN_WINDEPLOYQT=ON" }
# Only release-mode builds: a development build (MUSE_APP_UNSTABLE) keeps the QA hooks (COLLAB_AUTOBOT,
# COLLAB_HOST, PLOQ_QA_*) that release builds compile out, and must never ship to users.
$frameworkConfig = Join-Path (Split-Path $InstallDir -Parent) "muse_framework_config.h"
if (-not (Test-Path $frameworkConfig)) { throw "Can't tell the build mode: $frameworkConfig is missing" }
if (Select-String -Path $frameworkConfig -Pattern '^\s*#define\s+MUSE_APP_UNSTABLE\b' -Quiet) {
    throw "This is a development build (MUSE_APP_UNSTABLE). Build in release mode: -DMUSE_APP_BUILD_MODE=release (see README.md)"
}

Write-Host "1/5 Staging Ploq $version..."
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $obj) { Remove-Item -Recurse -Force $obj }
New-Item -ItemType Directory -Force $stage, $obj | Out-Null
# Debug symbols (*.pdb) stay out of the installer (they double its size).
robocopy $InstallDir $stage /E /XF *.pdb *.ilk /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
Copy-Item "$root\tools\collab\portable\Open-Link.ps1" "$stage\Open-Link.ps1"
Copy-Item "$root\LICENSE.txt" "$stage\LICENSE.txt"
# Next to LICENSE.txt: the third-party notices and the Inter font's licence (OFL 1.1
# asks for it to go with the font, which is built into Ploq.exe).
Copy-Item "$PSScriptRoot\THIRD_PARTY_NOTICES.txt" "$stage\THIRD_PARTY_NOTICES.txt"
Copy-Item "$root\src\collab\fonts\Inter-LICENSE.txt" "$stage\Inter-LICENSE.txt"

# Keep this release's symbols: a user's crash dump (%LOCALAPPDATA%\Ploq\crashes) only shows a
# readable stack with the exact Ploq.pdb + Ploq.exe of their version, and the next build
# overwrites both. Open a dump in WinDbg / Visual Studio with builds\symbols\<version> as the
# symbol and image path.
$pdb = Join-Path (Split-Path $InstallDir -Parent) "Ploq.pdb"
$symbols = "$root\builds\symbols\$version"
if (Test-Path $pdb) {
    New-Item -ItemType Directory -Force $symbols | Out-Null
    Copy-Item $pdb, "$InstallDir\bin\Ploq.exe" $symbols -Force
    Write-Host "    symbols kept in $symbols"
} else {
    Write-Warning "No Ploq.pdb next to ${InstallDir}: crash dumps from $version won't have a readable stack"
}

Write-Host "2/5 Code signing the program files..."
& "$PSScriptRoot\sign.ps1" -Files (Get-ChildItem $stage -Recurse -Include *.exe, *.dll).FullName

Write-Host "3/5 Collecting files (heat)..."
& "$Wix\heat.exe" dir $stage -nologo -cg PloqFiles -dr INSTALLDIR -srd -sreg -scom -gg -sfrag -var var.SourceDir -out "$obj\files.wxs"
if ($LASTEXITCODE -ne 0) { throw "heat failed" }

Write-Host "4/5 Building the .msi (candle + light)..."
$license = "$PSScriptRoot\License.rtf"
& "$Wix\candle.exe" -nologo -arch x64 -ext WixUtilExtension -out "$obj\" `
    "-dPloqVersion=$version" "-dSourceDir=$stage" "-dIconFile=$root\share\icons\Ploq\Ploq.ico" "-dLicenseRtf=$license" `
    "$PSScriptRoot\Ploq.wxs" "$obj\files.wxs"
if ($LASTEXITCODE -ne 0) { throw "candle failed" }
# ICE38/64/91: per-user installs keep files under %LOCALAPPDATA%, which these
# Program-Files-oriented checks flag; the HKCU key path in Ploq.wxs covers it.
& "$Wix\light.exe" -nologo -ext WixUIExtension -ext WixUtilExtension -cultures:en-us `
    -sice:ICE38 -sice:ICE64 -sice:ICE91 -spdb -out $msi "$obj\Ploq.wixobj" "$obj\files.wixobj"
if ($LASTEXITCODE -ne 0) { throw "light failed" }

Write-Host "5/5 Code signing the installer..."
& "$PSScriptRoot\sign.ps1" -Files @($msi)

$mb = [math]::Round((Get-Item $msi).Length / 1MB)
Write-Host "Done: $msi ($mb MB)"
Write-Host "Publish it as an update: node tools\collab\installer\make_release.mjs $msi"
