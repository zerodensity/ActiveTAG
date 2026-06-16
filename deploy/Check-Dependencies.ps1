[CmdletBinding()]
param(
    [switch]$RepairWindows
)

$ErrorActionPreference = "Stop"

$requiredDlls = @(
    "KERNEL32.dll",
    "USER32.dll",
    "COMDLG32.dll",
    "ADVAPI32.dll",
    "D3D11.dll",
    "D3DCOMPILER_47.dll",
    "IMM32.dll",
    "SHELL32.dll"
)

Write-Host "ActiveTAG Configurator dependency check" -ForegroundColor Cyan
Write-Host "Windows: $([Environment]::OSVersion.VersionString)"
Write-Host "Architecture: $env:PROCESSOR_ARCHITECTURE"
Write-Host ""

if (-not [Environment]::Is64BitOperatingSystem) {
    Write-Error "ActiveTAG Configurator requires 64-bit Windows."
}

$missing = @()
foreach ($dll in $requiredDlls) {
    $path = Join-Path $env:SystemRoot "System32\$dll"
    if (Test-Path -LiteralPath $path) {
        Write-Host "[OK] $dll" -ForegroundColor Green
    } else {
        Write-Host "[MISSING] $dll" -ForegroundColor Red
        $missing += $dll
    }
}

if ($missing.Count -eq 0) {
    Write-Host ""
    Write-Host "All required Windows system components are present." -ForegroundColor Green
    Write-Host "No DLL copying or registration is required."
    exit 0
}

Write-Host ""
Write-Warning "Protected Windows system components are missing."
Write-Warning "Do not download individual DLL files or copy them from another computer."

if (-not $RepairWindows) {
    Write-Host ""
    Write-Host "Run this script as Administrator with -RepairWindows to start the official repair:" -ForegroundColor Yellow
    Write-Host "  powershell -ExecutionPolicy Bypass -File .\Check-Dependencies.ps1 -RepairWindows"
    exit 1
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdministrator = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
)

if (-not $isAdministrator) {
    Write-Error "Windows repair requires an Administrator PowerShell window."
}

Write-Host ""
Write-Host "Running Windows component repair (DISM)..." -ForegroundColor Yellow
& "$env:SystemRoot\System32\dism.exe" /Online /Cleanup-Image /RestoreHealth
if ($LASTEXITCODE -ne 0) {
    throw "DISM failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "Running protected system file repair (SFC)..." -ForegroundColor Yellow
& "$env:SystemRoot\System32\sfc.exe" /scannow
if ($LASTEXITCODE -ne 0) {
    throw "SFC failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "Repair completed. Restart Windows, then run this check again." -ForegroundColor Green
