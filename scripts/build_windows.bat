@echo off
rem Build synccap on Windows with MSVC + Ninja from the Visual Studio installation.
rem usage: scripts\build_windows.bat [Release|Debug] [extra cmake args...]
setlocal
set VSLANG=1033
set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Release
rem paths are absolute, so the script works from any directory
for %%i in ("%~dp0..") do set REPO_ROOT=%%~fi
set BUILD_DIR=%REPO_ROOT%\build

rem a running program keeps its exe locked and the link would fail with LNK1104
for %%p in (synccap_collector synccap_server synccap_dashboard synccap_tests) do (
    tasklist /FI "IMAGENAME eq %%p.exe" /NH 2>nul | find /I "%%p.exe" >nul && (
        echo %%p.exe is running. Stop it first: the build cannot replace a running executable.
        exit /b 1
    )
)

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSROOT=%%i
if not defined VSROOT set VSROOT=%ProgramFiles%\Microsoft Visual Studio\18\Community
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "%REPO_ROOT%\3rdparty\ffmpeg_x264\win64\lib\avcodec.lib" (
    echo FFmpeg not found, downloading the pinned build...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fetch_ffmpeg.ps1" || exit /b 1
)

set VSCMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set VSCTEST=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe
if not exist "%VSCMAKE%" set VSCMAKE=cmake
if not exist "%VSCTEST%" set VSCTEST=ctest

"%VSCMAKE%" -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %SYNCCAP_CMAKE_ARGS% || exit /b 1
"%VSCMAKE%" --build "%BUILD_DIR%" || exit /b 1
if "%SYNCCAP_SKIP_TESTS%"=="1" exit /b 0
rem ctest runs inside the build directory: --test-dir needs CMake 3.20, the minimum here is 3.16.
rem setlocal restores the caller's directory when the script ends
cd /d "%BUILD_DIR%" || exit /b 1
"%VSCTEST%" --output-on-failure --timeout 300 || exit /b 1
endlocal
