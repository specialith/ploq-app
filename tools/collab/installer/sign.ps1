# Ploq - Windows code signing (Authenticode). THE place to turn signing on.
#
# Two ways, tried in this order; with neither set up, files stay unsigned (and it says so):
#
#  A. Azure Artifact Signing (formerly Trusted Signing): Microsoft-issued certificates for
#     the verified publisher ("Ploq, LLC"), renewed every 3 days by the service.
#     One-time setup: see tools/collab/installer/README.md, "Code signing". Then:
#       - %USERPROFILE%\.ploq\artifact-signing.json (or PLOQ_SIGN_METADATA = its path):
#           { "Endpoint": "https://<region>.codesigning.azure.net",
#             "CodeSigningAccountName": "<account>", "CertificateProfileName": "<profile>",
#             "ExcludeCredentials": [ ...optional... ] }
#       - signed in to Azure (az login) as someone with the
#         "Artifact Signing Certificate Profile Signer" role on that profile.
#     The client (Microsoft.ArtifactSigning.Client, from NuGet) is fetched into
#     %USERPROFILE%\tools\artifact-signing on first use (PLOQ_SIGN_DLIB overrides).
#
#  B. A certificate in the Windows certificate store (or a hardware token):
#     PLOQ_SIGN_CERT_THUMBPRINT = its thumbprint.
#
# Only files without a valid signature are signed: the Qt and runtime DLLs that ship
# signed by their makers keep their own signatures, and the signing quota isn't spent
# on them (a release is Ploq.exe + the .msi). Every signature is checked afterwards.
# Once releases are signed, make the updater insist on it: define PLOQ_REQUIRE_AUTHENTICODE
# in src/collab/CMakeLists.txt (see verifyAuthenticode in ploqupdate.cpp).

param([string[]]$Files = @())

$metadata = if ($env:PLOQ_SIGN_METADATA) { $env:PLOQ_SIGN_METADATA } else { Join-Path $env:USERPROFILE ".ploq\artifact-signing.json" }
$useArtifact = Test-Path $metadata
$thumb = $env:PLOQ_SIGN_CERT_THUMBPRINT
if (-not $useArtifact -and -not $thumb) {
    Write-Host "   (not signed: no signing set up - see tools\collab\installer\sign.ps1)"
    return
}

$missing = @($Files | Where-Object { $_ -and -not (Test-Path -LiteralPath $_ -PathType Leaf) })
if ($missing.Count) { throw "files to sign don't exist: $($missing -join ', ')" }
$todo = @($Files | Where-Object { $_ -and (Get-AuthenticodeSignature $_).Status -ne 'Valid' })
$kept = $Files.Count - $todo.Count
if ($todo.Count -eq 0) {
    Write-Host "   nothing to sign ($kept file(s) already signed by their makers)"
    return
}

# SignTool 10.0.22621.755 or newer (the Artifact Signing client needs it; the 20348 SDK's doesn't work).
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '10\.0\.20348' } |
    Sort-Object { [version]($_.FullName -replace '.*\\bin\\(10\.[\d.]+)\\.*', '$1') } -Descending | Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found (install the Windows 11 SDK)" }

if ($useArtifact) {
    $dlib = $env:PLOQ_SIGN_DLIB
    if (-not $dlib) {
        # 1.0.146 is the newest client on .NET 8; 1.0.151+ needs the .NET 10 runtime (without it,
        # signtool crashes on load with no message). Set PLOQ_SIGN_DLIB_VERSION to move on.
        $ver = if ($env:PLOQ_SIGN_DLIB_VERSION) { $env:PLOQ_SIGN_DLIB_VERSION } else { "1.0.146" }
        $dir = Join-Path $env:USERPROFILE "tools\artifact-signing"
        $dlib = Join-Path $dir "pkg-$ver\bin\x64\Azure.CodeSigning.Dlib.dll"
        if (-not (Test-Path $dlib)) {
            Write-Host "   fetching Microsoft.ArtifactSigning.Client $ver from NuGet..."
            New-Item -ItemType Directory -Force $dir | Out-Null
            $pkg = Join-Path $dir "pkg-$ver.nupkg"
            Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.artifactsigning.client/$ver/microsoft.artifactsigning.client.$ver.nupkg" -OutFile $pkg -UseBasicParsing
            Expand-Archive $pkg (Join-Path $dir "pkg-$ver") -Force
        }
    }
    $who = (Get-AuthenticodeSignature $dlib).SignerCertificate.Subject
    if ((Get-AuthenticodeSignature $dlib).Status -ne 'Valid' -or $who -notmatch 'O=Microsoft Corporation') {
        throw "$dlib isn't signed by Microsoft; refusing to use it"
    }
    $how = @("/fd", "SHA256", "/tr", "http://timestamp.acs.microsoft.com", "/td", "SHA256", "/dlib", $dlib, "/dmdf", $metadata)
    Write-Host "   signing with Azure Artifact Signing ($((Get-Content $metadata -Raw | ConvertFrom-Json).CodeSigningAccountName))"
} else {
    $timestamp = if ($env:PLOQ_SIGN_TIMESTAMP_URL) { $env:PLOQ_SIGN_TIMESTAMP_URL } else { "http://timestamp.digicert.com" }
    $how = @("/sha1", $thumb, "/fd", "sha256", "/tr", $timestamp, "/td", "sha256")
}

# signtool takes many files per call; keep command lines short.
for ($i = 0; $i -lt $todo.Count; $i += 50) {
    $batch = $todo[$i..([math]::Min($i + 49, $todo.Count - 1))]
    & $signtool.FullName sign @how /q @batch
    if ($LASTEXITCODE -ne 0) { throw "signtool failed (exit $LASTEXITCODE)" }
}
$bad = @($todo | Where-Object { (Get-AuthenticodeSignature $_).Status -ne 'Valid' })
if ($bad.Count) { throw "these didn't end up validly signed: $($bad -join ', ')" }
$signer = ((Get-AuthenticodeSignature $todo[0]).SignerCertificate.Subject -split ',')[0]
Write-Host "   signed $($todo.Count) file(s) as $signer ($kept already signed by their makers)"
