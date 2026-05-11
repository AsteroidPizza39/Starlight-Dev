@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
cd /d "%ROOT%"

call "%ROOT%scripts\vswin-init.bat"
if errorlevel 1 goto :fail

echo [1/2] Configuring preset x64-debug...
"%CMAKE_EXE%" --preset x64-debug
if errorlevel 1 goto :fail

echo [2/2] Building...
"%CMAKE_EXE%" --build --preset x64-debug
if errorlevel 1 goto :fail

set "OUT_EXE=%ROOT%out\build\x64-debug\src\Starlight.exe"
if not exist "%OUT_EXE%" (
    echo [ERROR] Build finished but exe was not found at:
    echo         "%OUT_EXE%"
    goto :fail
)

if not exist "%ROOT%rootDir" mkdir "%ROOT%rootDir"
copy /Y "%OUT_EXE%" "%ROOT%rootDir\Starlight.exe"
if errorlevel 1 goto :fail

echo.
echo Done. Starlight.exe copied to: rootDir\Starlight.exe
echo Build tree: out\build\x64-debug\
if /i not "%~1"=="nopause" pause
exit /b 0

:fail
echo.
echo [FAILED] build-debug.bat — see errors above.
if /i not "%~1"=="nopause" pause
exit /b 1
