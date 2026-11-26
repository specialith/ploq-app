@echo off
REM Ploq build wrapper (Windows).
REM Loads the Visual Studio C++ environment (including Preview/Insiders editions,
REM which upstream's ninja_build.bat skips), then runs the official build.cmake.
REM Usage (from the repo root):  tools\collab\build.bat            -> configure + build + install
REM                      tools\collab\build.bat run        -> launch the built app
REM Personal settings (Qt path, Ninja) live in build_overrides.cmake.

SET VSWHERE="C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
FOR /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  SET VS_INSTALL_DIR=%%i
)
IF "%VS_INSTALL_DIR%" == "" (
  ECHO error: no Visual Studio with C++ x64 build tools found
  EXIT /b 1
)
ECHO VS_INSTALL_DIR: %VS_INSTALL_DIR%
CALL "%VS_INSTALL_DIR%\VC\Auxiliary\Build\vcvars64.bat" >NUL || EXIT /b 1

cd /d "%~dp0\..\.."
cmake -P build.cmake %*
