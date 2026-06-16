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

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Generate-Icon.ps1"
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Prepare-Build.ps1"
if errorlevel 1 exit /b 1

for /f "usebackq tokens=*" %%i in (`powershell.exe -NoProfile -Command "$v=Get-Content -Raw '%~dp0version.json'|ConvertFrom-Json; 'v{0}.{1}.{2}' -f $v.major,$v.minor,$v.patch"`) do (
  set "APP_VERSION=%%i"
)

if not exist build mkdir build
pushd build

rc.exe /nologo /fo "ActiveTAG-Configurator.res" "..\src\generated\app.rc"
if errorlevel 1 (
  popd
  exit /b 1
)

cl /nologo /std:c++20 /O2 /W4 /EHsc /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /MT ^
  /I"..\third_party" /I"..\third_party\imgui" /I"..\third_party\imgui\backends" ^
  "..\src\main_imgui.cpp" "..\src\active_tag.cpp" "..\src\serial_port.cpp" ^
  "..\third_party\imgui\imgui.cpp" ^
  "..\third_party\imgui\imgui_draw.cpp" ^
  "..\third_party\imgui\imgui_tables.cpp" ^
  "..\third_party\imgui\imgui_widgets.cpp" ^
  "..\third_party\imgui\backends\imgui_impl_win32.cpp" ^
  "..\third_party\imgui\backends\imgui_impl_dx11.cpp" ^
  "ActiveTAG-Configurator.res" ^
  /link /SUBSYSTEM:WINDOWS /OUT:"ActiveTAG-Configurator-%APP_VERSION%.exe" ^
  user32.lib gdi32.lib comdlg32.lib advapi32.lib d3d11.lib dxgi.lib imm32.lib

set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
