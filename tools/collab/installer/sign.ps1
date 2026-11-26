# Ploq - Windows code signing (Authenticode). THE place to turn signing on.
#
# Until the certificate arrives this does nothing (and says so). To turn it on:
#   1. Install the code-signing certificate in your Windows certificate store
#      (or have the hardware token plugged in).
#   2. Set PLOQ_SIGN_CERT_THUMBPRINT to its thumbprint (certmgr.msc > the
#      certificate > Details > Thumbprint), e.g. in your user environment.
#   3. Rebuild the installer: Ploq.exe, its DLLs and the .msi get signed and
#      timestamped.
#   4. Then make the updater insist on it: define PLOQ_REQUIRE_AUTHENTICODE in
#      src/collab/CMakeLists.txt (see verifyAuthenticode in ploqupdate.cpp).

param([string[]]$Files = @())

$thumb = $env:PLOQ_SIGN_CERT_THUMBPRINT
if (-not $thumb) {
    Write-Host "   (not signed: no certificate configured - see tools\collab\installer\sign.ps1)"
    return
}

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found (install the Windows SDK)" }
$timestamp = if ($env:PLOQ_SIGN_TIMESTAMP_URL) { $env:PLOQ_SIGN_TIMESTAMP_URL } else { "http://timestamp.digicert.com" }

# signtool takes many files per call; keep command lines short.
for ($i = 0; $i -lt $Files.Count; $i += 50) {
    $batch = $Files[$i..([math]::Min($i + 49, $Files.Count - 1))]
    & $signtool.FullName sign /sha1 $thumb /fd sha256 /tr $timestamp /td sha256 /q @batch
    if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
}
Write-Host "   signed $($Files.Count) file(s)"
