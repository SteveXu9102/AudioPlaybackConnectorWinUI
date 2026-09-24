<#
.SYNOPSIS
    Builds and packages one single-file distribution.

.DESCRIPTION
    Builds the application in the requested deployment model, bundles it into the
    single-file launcher and writes

        dist\<Platform>\AudioPlaybackConnectorWinUI-<Platform>-<Deployment>.exe

    The four distributions of a release are the two architectures crossed with the
    two deployment models:

        -Platform x64   -Deployment SelfContained
        -Platform x64   -Deployment FrameworkDependent
        -Platform ARM64 -Deployment SelfContained
        -Platform ARM64 -Deployment FrameworkDependent

    Both deployment models build into the same output directory (the project's
    $(Platform)\$(Configuration)), so the application output and its intermediate
    files are deleted before every build. Without that, a framework-dependent
    build would pick up the Windows App SDK binaries the previous self-contained
    build left behind. tools/package-single-file.ps1 checks the result as well.

.PARAMETER Deployment
    SelfContained carries the Windows App SDK binaries and needs nothing else on
    the machine. FrameworkDependent needs the Windows App Runtime 2.5.1 installed.

.PARAMETER MSBuild
    Path to MSBuild.exe. By default it is taken from PATH, then from vswhere.

.PARAMETER RestoreSources
    Overrides the NuGet sources, for example a local package folder when building
    offline.

.EXAMPLE
    pwsh tools/build-single-file.ps1 -Platform x64 -Deployment SelfContained
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('SelfContained', 'FrameworkDependent')]
    [string]$Deployment = 'SelfContained',
    [string]$Configuration = 'Release',
    [string]$MSBuild = '',
    [string]$RestoreSources = '',
    [switch]$SkipRestore
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

function Resolve-MSBuild {
    param([string]$Explicit)

    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) {
            throw "MSBuild not found: $Explicit"
        }
        return $Explicit
    }

    $command = Get-Command 'msbuild.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    $candidates = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\*\*\MSBuild\Current\Bin\MSBuild.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending
    if ($candidates) {
        return $candidates[0].FullName
    }

    throw 'MSBuild.exe was not found. Pass -MSBuild or build from a Developer Command Prompt.'
}

$msbuildPath = Resolve-MSBuild -Explicit $MSBuild
$selfContained = $Deployment -eq 'SelfContained'

# Every build of the application writes here, whichever deployment model it uses.
$appOutDir = Join-Path $root "$Platform\$Configuration"
$appObjDir = Join-Path $root "$Platform\obj\$Configuration"

$common = @(
    '-nologo'
    '-m'
    "-p:Configuration=$Configuration"
    "-p:Platform=$Platform"
    # The sandboxed environments the project is built in do not react well to
    # MSBuild's file-access tracking tables.
    '-p:TrackFileAccess=false'
)
if ($RestoreSources) {
    $common += "-p:RestoreSources=$RestoreSources"
}

Write-Host "== $Platform $Deployment =="

if (-not $SkipRestore) {
    & $msbuildPath (Join-Path $root 'AudioPlaybackConnectorWinUI.sln') -t:Restore @common
    if ($LASTEXITCODE -ne 0) {
        throw "Restore failed with exit code $LASTEXITCODE."
    }
}

# The complete third-party notices - the component list plus every redistributed
# package's own licence and notice text - are assembled before anything is compiled,
# because the resource script embeds them as RCDATA (301). What is embedded is the
# compressed form the assembly writes beside the text, because the text is about
# 889 KB and its MSZIP stream about 92 KB. The text itself is what
# tools\package-single-file.ps1 copies beside each packaged executable, and the
# dialog decompresses the embedded stream in memory, so the text the program shows
# and the text it ships are the same bytes. It is assembled after the restore
# because the packages and their versions come from obj\project.assets.json, and the
# root copies it writes are listed in .gitignore.
& (Join-Path $PSScriptRoot 'assemble-third-party-notices.ps1') -Platform $Platform

# The launcher only differs per architecture, but it is cheap to rebuild and
# keeping it with the payload it belongs to avoids a stale launcher.
& $msbuildPath (Join-Path $root 'launcher\Launcher.vcxproj') @common
if ($LASTEXITCODE -ne 0) {
    throw "Launcher build failed with exit code $LASTEXITCODE."
}

foreach ($directory in @($appOutDir, $appObjDir)) {
    if (Test-Path -LiteralPath $directory) {
        Write-Host "cleaning $directory"
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
}

& $msbuildPath (Join-Path $root 'AudioPlaybackConnectorWinUI.vcxproj') @common "-p:WindowsAppSDKSelfContained=$($selfContained.ToString().ToLowerInvariant())"
if ($LASTEXITCODE -ne 0) {
    throw "Application build failed with exit code $LASTEXITCODE."
}

& (Join-Path $PSScriptRoot 'package-single-file.ps1') -Platform $Platform -Configuration $Configuration -Deployment $Deployment
