ActiveTAG Configurator - Portable Windows Package
=================================================

1. Run Run-Dependency-Check.cmd once.
2. If every dependency is marked OK, run ActiveTAG-Configurator.exe.
3. No installation, DLL copying, or DLL registration is required.
4. Keep the EXE in a writable folder. ActiveTAG-Configurator.log is created
   next to the EXE and appended for every application session.

Required DLLs:
  KERNEL32.dll
  USER32.dll
  GDI32.dll
  COMCTL32.dll
  COMDLG32.dll
  ADVAPI32.dll

These are protected Windows system components. They are part of supported
Windows installations and are not redistributable application DLLs.

Do not:
  - Download individual DLLs from third-party DLL websites.
  - Copy these DLLs from another Windows computer.
  - Copy them into System32 or SysWOW64.
  - Run regsvr32 against them.

If the dependency check reports a missing DLL, open PowerShell as
Administrator in this folder and run:

  powershell -ExecutionPolicy Bypass -File .\Check-Dependencies.ps1 -RepairWindows

The repair option uses the official Windows DISM and SFC tools. Restart the
computer after repair.
