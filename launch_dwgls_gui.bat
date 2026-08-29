@echo off
REM DWGLS GUI Launcher
REM ════════════════════════════════════════════════════════════════════════════
REM Serves DWGLS GUI at http://localhost:8081
REM Requires Python 3.x and DWGLS executables in build/

cd /d "%~dp0"

echo DWGLS GUI v1.0.0
echo Checking executables...

for %%f in (geo_rid_graft.exe geofs_rid.exe kv_rid_serve.exe gguf_roundtrip.exe) do (
    if exist build\%%f (
        echo   %%f  [OK]
    ) else (
        echo   %%f  [MISSING] - run: gcc -O2 -std=c11 -Wall -I core -o build\%%f tools\%%~nf.c -lm (with llama.dll for graft targets)
    )
)

echo.
echo Starting server...
python dwgls_gui_server.py

pause