@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VSROOT=%%i"
)

if not defined VSROOT exit /b 1
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

if not exist build mkdir build
cl /nologo /std:c++20 /O2 /W4 /EHsc /permissive- /DUNICODE /D_UNICODE /DNOMINMAX /MT ^
  "test\native_tests.cpp" "src\active_tag.cpp" "src\serial_port.cpp" ^
  /Fe:"build\ActiveTAG-NativeTests.exe" ^
  /link advapi32.lib
if errorlevel 1 exit /b 1

"build\ActiveTAG-NativeTests.exe"
