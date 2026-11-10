@echo off
rem Builds renderer\build\Release\ddraw.dll and the installer
rem installer\Output\RoadRashFFNNOPatch-Setup-<version>.exe.
rem ISCC lookup: %ISCC% (override), then %LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe,
rem then %ProgramFiles(x86)%\Inno Setup 6\ISCC.exe.
setlocal

for %%I in ("%~dp0..") do set "ROOT=%%~fI"

call "%ROOT%\renderer\build.bat"
if errorlevel 1 (
  echo installer\build.bat: renderer build failed
  exit /b 1
)
if not exist "%ROOT%\renderer\build\Release\ddraw.dll" (
  echo installer\build.bat: ddraw.dll not found after the build
  exit /b 1
)

if defined ISCC goto have_iscc
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if exist "%ISCC%" goto have_iscc
set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ISCC%" goto have_iscc
echo installer\build.bat: ISCC.exe not found; install Inno Setup 6 or set ISCC
exit /b 1

:have_iscc
if not exist "%ISCC%" (
  echo installer\build.bat: ISCC not found at "%ISCC%"
  exit /b 1
)

"%ISCC%" /Q "%ROOT%\installer\RoadRashFFNNOPatch.iss"
if errorlevel 1 (
  echo installer\build.bat: ISCC failed
  exit /b 1
)

set "SETUP="
for /f "delims=" %%F in ('dir /b /o:d "%ROOT%\installer\Output\RoadRashFFNNOPatch-Setup-*.exe" 2^>nul') do set "SETUP=%ROOT%\installer\Output\%%F"
if not defined SETUP (
  echo installer\build.bat: setup exe not found in installer\Output
  exit /b 1
)
echo installer\build.bat: OK %SETUP%
exit /b 0
