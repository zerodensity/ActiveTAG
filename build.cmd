@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VSROOT=%%i"
)

if not defined VSROOT (
  echo Visual Studio C++ Build Tools bulunamadi.
  exit /b 1
)

call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

if not exist build mkdir build
pushd build

cl /nologo /std:c++20 /O2 /W4 /EHsc /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /MT ^
  /I"..\third_party" ^
  "..\src\main.cpp" "..\src\active_tag.cpp" "..\src\serial_port.cpp" ^
  /link /SUBSYSTEM:WINDOWS /OUT:"ActiveTAG-Configurator.exe" ^
  user32.lib gdi32.lib comctl32.lib comdlg32.lib advapi32.lib

set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
