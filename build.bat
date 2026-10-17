@echo off
rem Regenerate the recompiled code and build (RelWithDebInfo, into build/).
rem Usage: build.bat [configure]
cd /d "%~dp0"
call "%~dp0..\tools\build_env.bat" || exit /b 1
python tools\recomp.py data\vmunix recomp.toml generated || exit /b 1
if not exist build\build.ninja set CONFIGURE=1
if "%1"=="configure" set CONFIGURE=1
if defined CONFIGURE cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build build
