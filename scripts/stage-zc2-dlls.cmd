@echo off
REM stage-zc2-dlls.cmd — copy patched build_zc2 DLLs beside DWGLS exes.
REM exe-dir wins Windows DLL search order, killing PATH shadowing
REM (see docs/LLAMA-USER-BUFFER-PATCH.md L2 outage). Re-run after every
REM build_zc2 rebuild, then re-verify: plain 291/291 + lazy 12/12.
setlocal
set SRC=I:\llama\llama.cpp\build_zc2\bin\Release
set DST=I:\DWGLS-native-fs\build
if not exist "%DST%" mkdir "%DST%"
copy /Y "%SRC%\llama.dll" "%DST%\" || exit /b 1
copy /Y "%SRC%\ggml.dll" "%DST%\" || exit /b 1
copy /Y "%SRC%\ggml-base.dll" "%DST%\" || exit /b 1
copy /Y "%SRC%\ggml-cpu-*.dll" "%DST%\" || exit /b 1
echo staged:
dir /B "%DST%\llama.dll" "%DST%\ggml.dll" "%DST%\ggml-base.dll"
certutil -hashfile "%DST%\llama.dll" SHA256 | findstr /V ":"
