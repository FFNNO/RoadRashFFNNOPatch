@echo off
rem Run Ghidra headless import + export on the exe copy (never the Steam install).
set GHIDRA=C:\tools\ghidra\ghidra_12.1.4_PUBLIC
set ROOT=H:\ProyectosOpencode\mods-rr
rem Keep Ghidra settings/cache outside AppData: packaged hosts virtualize AppData and break Felix path checks.
set JAVA_TOOL_OPTIONS=-Dapplication.settingsdir=%ROOT%\ghidra-home\settings -Dapplication.cachedir=%ROOT%\ghidra-home\cache -Djava.io.tmpdir=%ROOT%\ghidra-home\tmp
if not exist "%ROOT%\ghidra-home\tmp" mkdir "%ROOT%\ghidra-home\tmp"
call "%GHIDRA%\support\analyzeHeadless.bat" "%ROOT%\ghidra" RoadRash95 -import "%ROOT%\original\RoadRash95.exe" -overwrite -scriptPath "%ROOT%\scripts" -postScript ExportAll.java "%ROOT%\out" > "%ROOT%\out\headless.log" 2>&1 < nul
