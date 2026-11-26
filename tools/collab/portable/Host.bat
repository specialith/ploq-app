@echo off
REM Host a score: shares it and copies an invite link to your clipboard - text
REM that link to whoever you want to edit with. Drag a .mscz file onto this
REM file, or double-click to use the demo score. In the app, Ctrl+Alt+I copies
REM another invite link any time. The first time, the app asks your name.
REM (Easier: share and invite on https://collab.ploq.app instead.)
setlocal
SET "BIN=%~dp0bin"
IF "%COLLAB_SERVER%" == "" SET "COLLAB_SERVER=https://collab.ploq.app"
SET "COLLAB_HOST=1"
REM Quoted assignment: a file name may contain "&" (file names can't contain quotes).
SET "SCORE=%~1"
IF "%SCORE%" == "" SET "SCORE=%~dp0demo\Amazing Grace.mscz"
IF "%COLLAB_COLOR%" == "" SET "COLLAB_COLOR=#e6194b"
REM Let ploq:// links open this copy (see Join.bat / Open-Link.ps1).
REM Remove the pre-rename collabscore:// handler (older packages pointed it at a vulnerable Join.bat).
reg delete "HKCU\Software\Classes\collabscore" /f >NUL 2>&1
SET "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
reg add "HKCU\Software\Classes\ploq" /ve /d "URL:Ploq link" /f >NUL
reg add "HKCU\Software\Classes\ploq" /v "URL Protocol" /d "" /f >NUL
reg add "HKCU\Software\Classes\ploq\shell\open\command" /ve /d "\"%PS%\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%~dp0Open-Link.ps1\" \"%%1\"" /f >NUL
DEL /Q "%LOCALAPPDATA%\Ploq\Ploq\session\session.json" 2>NUL
REM Old files are re-saved in the current format so nobody gets an upgrade dialog.
SET "WORK=%USERPROFILE%\Documents\Ploq"
MKDIR "%WORK%" 2>NUL
FOR %%f IN ("%SCORE%") DO SET "COPY=%WORK%\%%~nf.mscz"
IF NOT EXIST "%COPY%" (
  "%BIN%\Ploq.exe" -o "%COPY%" "%SCORE%" || (PAUSE & EXIT /b 1)
)
START "" "%BIN%\Ploq.exe" "%COPY%"
ECHO.
ECHO   Opening %COPY%
ECHO   When it's open, the invite link is copied to your clipboard - paste it
ECHO   into a text or chat. (Ctrl+Alt+I in the app copies another one.)
ECHO.
endlocal
