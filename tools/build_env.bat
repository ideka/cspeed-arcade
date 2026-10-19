@echo off
rem Sets up the build environment for build.bat: the MSVC environment, its bundled Ninja, and clang-cl.
rem
rem Machine-specific settings go in build_env.local.bat at the repository root (git-ignored), e.g.:
rem   set "LLVM_BIN=D:\LLVM\bin"    (default: %ProgramFiles%\LLVM\bin if it exists)

set "REPO_ROOT=%~dp0.."
if exist "%REPO_ROOT%\build_env.local.bat" call "%REPO_ROOT%\build_env.local.bat"

rem Visual Studio (MSVC environment and its bundled Ninja), located with vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio 2022 with the C++ workload is required ^(vswhere.exe not found^).
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_DIR=%%i"
if not defined VS_DIR (
    echo No Visual Studio installation with the C++ x64 tools was found.
    exit /b 1
)
call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

rem LLVM (clang-cl).
if not defined LLVM_BIN if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "LLVM_BIN=%ProgramFiles%\LLVM\bin"
if defined LLVM_BIN set "PATH=%LLVM_BIN%;%PATH%"
exit /b 0
