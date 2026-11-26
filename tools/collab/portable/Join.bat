@echo off
REM Run this once after unzipping: it lets "Open in Ploq" (on
REM https://collab.ploq.app) and invite links open this copy of the app.
REM
REM Links are handled by Open-Link.ps1, which treats them strictly as text.
REM This file never sees link text (a link run through cmd.exe could run
REM commands), so it takes no arguments.
setlocal
REM Remove the pre-rename collabscore:// handler (older packages pointed it at a vulnerable Join.bat).
reg delete "HKCU\Software\Classes\collabscore" /f >NUL 2>&1
SET "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
reg add "HKCU\Software\Classes\ploq" /ve /d "URL:Ploq link" /f >NUL
reg add "HKCU\Software\Classes\ploq" /v "URL Protocol" /d "" /f >NUL
reg add "HKCU\Software\Classes\ploq\shell\open\command" /ve /d "\"%PS%\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%~dp0Open-Link.ps1\" \"%%1\"" /f >NUL
ECHO.
ECHO   Ploq links are set up on this computer.
ECHO   Share or join scores at  https://collab.ploq.app
ECHO.
CHOICE /C YN /N /M "  Paste an invite link to open now? [Y/N] "
IF ERRORLEVEL 2 EXIT /b 0
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Open-Link.ps1"
endlocal
