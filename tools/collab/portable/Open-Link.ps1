# Opens a Ploq link in this copy of the app.
#
# Windows runs this for every ploq:// link (website "Open in Ploq", invite
# links, the browser sign-in's return trip). The link is only ever handled as a
# string: it must exactly match one of the shapes below or nothing happens.
# Never pass it through cmd.exe / a .bat file - "&" and quotes in a link would
# run commands.
#
# ploq://signin links never start Ploq: they go to the Ploq that is waiting
# for that exact sign-in (named pipe \\.\pipe\ploq-signin-<state>, opened by
# src/collab/internal/browsersignin.cpp). If none is waiting, nothing happens.
#
# tools/collab/qa-auto/linkshapes.ps1 dot-sources this file to test the shapes.
param([string]$Link = "")

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

#! What kind of Ploq link this is: "signin", "join", "open", "invite-url", or $null.
function Get-PloqLinkKind([string]$Text) {
    $shapes = [ordered]@{
        'signin'     = '^ploq://signin\?state=[0-9a-f]{32}&code=[0-9a-f]{32}/?\z'  # browser sign-in, back to the waiting app
        'join'       = '^ploq://join/[0-9a-f]{24}/?\z'                             # invite
        'open'       = '^ploq://open/p_[0-9a-f]{24}\?code=[0-9a-f]{32}/?\z'        # website "Open in Ploq"
        'invite-url' = '^https://collab\.ploq\.app/j/[0-9a-f]{24}/?\z'             # invite link pasted from a message
    }
    foreach ($kind in $shapes.Keys) {
        # -cmatch: case-sensitive, so "ploq://SIGNIN" or upper-case hex don't pass.
        # \z, not $: $ also matches before a final newline.
        if ($Text -cmatch $shapes[$kind]) { return $kind }
    }
    return $null
}

# Dot-sourced by the shape test: only define the function.
if ($MyInvocation.InvocationName -eq '.') { return }

function Show-Message([string]$text) {
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show($text, "Ploq")
}

if ([string]::IsNullOrWhiteSpace($Link)) {
    $Link = Read-Host "Paste the invite link"
}
$Link = $Link.Trim()

$kind = Get-PloqLinkKind $Link
if (-not $kind) {
    Show-Message "That isn't a Ploq link, so nothing was opened."
    exit 1
}

if ($kind -eq "signin") {
    # Hand the link to the Ploq that started this sign-in. It checks the link
    # again, asks "Sign in to Ploq as ...?" and redeems the code.
    $state = [regex]::Match($Link, 'state=([0-9a-f]{32})').Groups[1].Value
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "ploq-signin-$state", [System.IO.Pipes.PipeDirection]::Out)
    try {
        $pipe.Connect(3000)
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($Link + "`n")
        $pipe.Write($bytes, 0, $bytes.Length)
    } catch {
        $pipe.Dispose()
        Show-Message "This sign-in has expired, or it wasn't started in Ploq on this computer. In Ploq, click Share to sign in again."
        exit 1
    }
    # Ploq reads the one line and hangs up straight away, so waiting for the
    # pipe to drain can fail with "pipe is broken" after a successful hand-over.
    try { $pipe.Flush(); $pipe.WaitForPipeDrain() } catch { }
    $pipe.Dispose()
    exit 0
}

if (-not $env:COLLAB_SERVER) { $env:COLLAB_SERVER = "https://collab.ploq.app" }
if (-not $env:COLLAB_COLOR) { $env:COLLAB_COLOR = "#f58231" }
$env:COLLAB_INVITE = $Link

# Forget the last session so MuseScore doesn't ask "restore the session?".
Remove-Item -ErrorAction SilentlyContinue "$env:LOCALAPPDATA\Ploq\Ploq\session\session.json"

Start-Process -FilePath (Join-Path $here "bin\Ploq.exe")
