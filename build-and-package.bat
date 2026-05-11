@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
cd /d "%ROOT%"

call "%ROOT%scripts\vswin-init.bat"
if errorlevel 1 goto :fail

if not exist "%CPACK_EXE%" (
    echo [ERROR] cpack.exe not found beside CMake. Add VS component: "C++ CMake tools for Windows"
    goto :fail
)

echo [1/4] Configuring x64-release...
"%CMAKE_EXE%" --preset x64-release
if errorlevel 1 goto :fail

echo [2/4] Building...
"%CMAKE_EXE%" --build --preset x64-release
if errorlevel 1 goto :fail

set "BUILD_DIR=%ROOT%out\build\x64-release"
if not exist "%BUILD_DIR%\src\Starlight.exe" (
    echo [ERROR] Build finished but "%BUILD_DIR%\src\Starlight.exe" was not found.
    goto :fail
)

pushd "%BUILD_DIR%"
if errorlevel 1 goto :fail

echo [3/4] Creating portable ZIP package...
"%CPACK_EXE%" -C Release -G ZIP
if errorlevel 1 (
    popd
    goto :fail
)

echo [4/4] Creating NSIS installer...
"%CPACK_EXE%" -C Release -G NSIS
if errorlevel 1 (
    echo [WARN] NSIS installer generation failed.
    echo [WARN] Install NSIS and re-run: winget install NSIS.NSIS
    echo [WARN] ZIP package may still exist under "%BUILD_DIR%"
    popd
    exit /b 1
)

popd
echo [DONE] Build + packages completed successfully.
echo        Output folder: "%BUILD_DIR%"
exit /b 0

:fail
echo [FAILED] Build/package process failed.
exit /b 1
