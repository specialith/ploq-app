# Ploq - build the one-screen Windows installer: Ploq-<version>-Setup.exe, next to the .msi it carries.
#
#   powershell -File tools\collab\installer\setup\build_setup.ps1 -Msi builds\release\1.0.8\Ploq-1.0.8.msi
#   ... -Preview      # also draw every screen to PNGs next to the .exe (no window is shown)
#
# Needs nothing installed: the C# compiler and WPF come with Windows (.NET Framework 4.8). The .msi is
# embedded unchanged, so the setup installs exactly the release that was built and attested on GitHub.
# The .exe is signed like everything else once signing is set up (tools\collab\installer\sign.ps1).
param(
    [Parameter(Mandatory = $true)][string]$Msi,
    [string]$Out = "",
    [switch]$Preview
)
$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$root = (Resolve-Path "$here\..\..\..\..").Path
$Msi = (Resolve-Path $Msi).Path
if ($Msi -notmatch 'Ploq-(\d+\.\d+\.\d+)\.msi$') { throw "Expected a Ploq-x.y.z.msi, got $Msi" }
$version = $Matches[1]
$repoVersion = (Get-Content "$root\src\collab\PLOQ_VERSION" -TotalCount 1).Trim()
if ($version -ne $repoVersion) { Write-Warning "The .msi is $version but src\collab\PLOQ_VERSION says $repoVersion" }
if (-not $Out) { $Out = Join-Path (Split-Path $Msi) "Ploq-$version-Setup.exe" }
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$Out = [IO.Path]::GetFullPath($Out)

$fw = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$csc = "$fw\csc.exe"
if (-not (Test-Path $csc)) { throw "No C# compiler at $csc (.NET Framework 4.8 is part of Windows 10/11)" }

$obj = Join-Path $env:TEMP "ploq-setup-build"
New-Item -ItemType Directory -Force $obj | Out-Null
[IO.File]::WriteAllText("$obj\version.txt", $version)
# Version details shown in the file's Properties > Details.
$parts = $version.Split('.')
[IO.File]::WriteAllText("$obj\AssemblyInfo.cs", @"
using System.Reflection;
[assembly: AssemblyTitle("Ploq Setup")]
[assembly: AssemblyDescription("Installs Ploq $version")]
[assembly: AssemblyProduct("Ploq")]
[assembly: AssemblyCompany("Ploq, LLC")]
[assembly: AssemblyCopyright("Ploq, LLC. Ploq is free software under the GNU GPL v3.")]
[assembly: AssemblyVersion("$($parts[0]).$($parts[1]).$($parts[2]).0")]
[assembly: AssemblyFileVersion("$($parts[0]).$($parts[1]).$($parts[2]).0")]
[assembly: AssemblyInformationalVersion("$version")]
"@)

$icon = "$root\share\icons\Ploq\Ploq.ico"
$fonts = "$root\src\collab\fonts"
$art = "$here\setup-mark.png"
if (-not (Test-Path $art)) { throw "Missing $art (python tools\collab\installer\setup\make_setup_art.py)" }

Write-Host "Building $Out (Ploq $version)..."
& $csc /nologo /target:winexe /platform:anycpu /optimize+ /warnaserror+ `
    "/out:$Out" "/win32icon:$icon" "/win32manifest:$here\PloqSetup.manifest" `
    "/r:$fw\WPF\PresentationFramework.dll" "/r:$fw\WPF\PresentationCore.dll" "/r:$fw\WPF\WindowsBase.dll" `
    "/r:$fw\System.Xaml.dll" "/r:$fw\System.Windows.Forms.dll" `
    "/resource:$obj\version.txt,version.txt" "/resource:$icon,Ploq.ico" "/resource:$art,setup-mark.png" `
    "/resource:$fonts\Inter-Light.ttf,Inter-Light.ttf" "/resource:$fonts\Inter-Regular.ttf,Inter-Regular.ttf" `
    "/resource:$Msi,Ploq.msi" `
    "$here\PloqSetup.cs" "$obj\AssemblyInfo.cs"
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }

& "$root\tools\collab\installer\sign.ps1" -Files @($Out)

$mb = [math]::Round((Get-Item $Out).Length / 1MB)
Write-Host "Done: $Out ($mb MB)"

if ($Preview) {
    foreach ($state in "unticked", "ticked", "installing", "done", "error", "open") {
        $png = [IO.Path]::ChangeExtension($Out, $null).TrimEnd('.') + "-$state.png"
        $p = Start-Process -FilePath $Out -ArgumentList "--render", "`"$png`"", $state, "1" -Wait -PassThru -WindowStyle Hidden
        if ($p.ExitCode -ne 0) { throw "render $state failed ($($p.ExitCode))" }
        Write-Host "  preview: $png"
    }
}
