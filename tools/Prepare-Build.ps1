[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$version = Get-Content -Raw -LiteralPath (Join-Path $root "version.json") |
    ConvertFrom-Json

$major = [int]$version.major
$minor = [int]$version.minor
$patch = [int]$version.patch
$displayVersion = "v$major.$minor.$patch"
$numericVersion = "$major,$minor,$patch,0"
$fileVersion = "$major.$minor.$patch.0"
$generated = Join-Path $root "src\generated"

New-Item -ItemType Directory -Path $generated -Force | Out-Null

$header = @"
#pragma once

#define ACTIVETAG_VERSION_MAJOR $major
#define ACTIVETAG_VERSION_MINOR $minor
#define ACTIVETAG_VERSION_PATCH $patch
#define ACTIVETAG_VERSION_W L"$displayVersion"
#define ACTIVETAG_VERSION_A "$displayVersion"
#define ACTIVETAG_APP_TITLE_W L"ActiveTAG Configurator $displayVersion"
#define ACTIVETAG_WINDOW_TITLE_W L"Zero Density ActiveTAG Configurator $displayVersion"
#define ACTIVETAG_EXE_NAME_W L"ActiveTAG-Configurator-$displayVersion.exe"
"@
Set-Content -LiteralPath (Join-Path $generated "version.hpp") -Value $header -Encoding ascii

$resourceHeader = @"
#pragma once

#define IDI_ACTIVETAG 101
#define ACTIVETAG_VERSION_MAJOR $major
#define ACTIVETAG_VERSION_MINOR $minor
#define ACTIVETAG_VERSION_PATCH $patch
"@
Set-Content -LiteralPath (Join-Path $generated "resource.h") -Value $resourceHeader -Encoding ascii

$resource = @"
#include <windows.h>
#include "resource.h"

IDI_ACTIVETAG ICON "../../assets/ActiveTAG-Configurator.ico"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION $numericVersion
 PRODUCTVERSION $numericVersion
 FILEFLAGSMASK 0x3fL
#ifdef _DEBUG
 FILEFLAGS 0x1L
#else
 FILEFLAGS 0x0L
#endif
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "ActiveTAG Project\0"
            VALUE "FileDescription", "Native OptiTrack Active Tag Configurator\0"
            VALUE "FileVersion", "$fileVersion\0"
            VALUE "InternalName", "ActiveTAG-Configurator\0"
            VALUE "OriginalFilename", "ActiveTAG-Configurator-$displayVersion.exe\0"
            VALUE "ProductName", "ActiveTAG Configurator\0"
            VALUE "ProductVersion", "$displayVersion\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
"@
Set-Content -LiteralPath (Join-Path $generated "app.rc") -Value $resource -Encoding ascii

Write-Host "Prepared ActiveTAG Configurator $displayVersion build metadata."
