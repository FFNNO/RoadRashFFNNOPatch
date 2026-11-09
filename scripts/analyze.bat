@echo off
rem Run Ghidra headless import + export on the exe copy (never the Steam install).
rem Set GHIDRA_DIR to your Ghidra install (the folder that contains support\analyzeHeadless.bat).
if "%GHIDRA_DIR%"=="" (
  echo analyze.bat: set GHIDRA_DIR to your Ghidra install, for example C:\tools\ghidra\ghidra_12.1.4_PUBLIC
  exit /b 1
)
set GHIDRA=%GHIDRA_DIR%
for %%I in ("%~dp0..") do set ROOT=%%~fI
rem Keep Ghidra settings/cache outside AppData: packaged hosts virtualize AppData and break Felix path checks.
set JAVA_TOOL_OPTIONS=-Dapplication.settingsdir=%ROOT%\ghidra-home\settings -Dapplication.cachedir=%ROOT%\ghidra-home\cache -Djava.io.tmpdir=%ROOT%\ghidra-home\tmp
if not exist "%ROOT%\ghidra-home\tmp" mkdir "%ROOT%\ghidra-home\tmp"
if not exist "%ROOT%\out" mkdir "%ROOT%\out"
call "%GHIDRA%\support\analyzeHeadless.bat" "%ROOT%\ghidra" RoadRash95 -import "%ROOT%\original\RoadRash95.exe" -overwrite -scriptPath "%ROOT%\scripts" -postScript ExportAll.java "%ROOT%\out" > "%ROOT%\out\headless.log" 2>&1 < nul
