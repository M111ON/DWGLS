@echo off
set PATH=%~dp0dll;%PATH%
cd /d %~dp0
echo DWGLS Portable — KV Flow Demo
echo ============================
echo.
bin\kv_flow_demo.exe config.json
echo.
pause