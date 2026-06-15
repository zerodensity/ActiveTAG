[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$version = Get-Content -Raw -LiteralPath (Join-Path $root "version.json") |
    ConvertFrom-Json
$displayVersion = "v$($version.major).$($version.minor).$($version.patch)"
$exeName = "ActiveTAG-Configurator-$displayVersion.exe"
$exe = Join-Path $root "build\$exeName"
$packageRoot = Join-Path $root "dist\ActiveTAG-Configurator-$displayVersion-Portable"
$zipPath = Join-Path $root "dist\ActiveTAG-Configurator-$displayVersion-Portable-x64.zip"

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
