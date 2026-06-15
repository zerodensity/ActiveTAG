[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$exe = Join-Path $root "build\ActiveTAG-Configurator.exe"
$packageRoot = Join-Path $root "dist\ActiveTAG-Configurator-Portable"
$zipPath = Join-Path $root "dist\ActiveTAG-Configurator-Portable-x64.zip"

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build output not found. Run build.cmd first."
}

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $root "deploy\Check-Dependencies.ps1") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $root "deploy\Run-Dependency-Check.cmd") -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $root "deploy\README.txt") -Destination $packageRoot

Compress-Archive -LiteralPath $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host "Portable package created: $zipPath"
