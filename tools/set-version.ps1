<#
.SYNOPSIS
    Reads, verifies or writes the version every released file carries.

.DESCRIPTION
    The version lives in four files: the application's resource script, the
    launcher's resource script, the application manifest and the launcher's
    manifest.

    This script is the single place that changes them, and the check that keeps
    them agreeing:

        pwsh tools/set-version.ps1 -Version 1.4.0        write it everywhere
        pwsh tools/set-version.ps1 -Verify                assert the four agree
        pwsh tools/set-version.ps1 -Verify -Version 1.4.0

    Writing a version a file already carries is a success, not a failure: a tag
    whose version was committed by hand is the normal case, and re-running the
    stamp has to be harmless. Only a version line that cannot be found at all is
    an error.

.PARAMETER Version
    "major.minor.build" or "major.minor.build.revision".

.PARAMETER Verify
    Changes nothing. Fails when the files disagree with each other, or when they
    do not match -Version.
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [switch]$Verify
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

$resourceFiles = @(
    (Join-Path $root 'AudioPlaybackConnectorWinUI.rc'),
    (Join-Path $root 'launcher\Launcher.rc')
)
$manifestFiles = @(
    (Join-Path $root 'AudioPlaybackConnectorWinUI.manifest'),
    (Join-Path $root 'launcher\Launcher.manifest')
)

# The resource scripts are UTF-16LE with a BOM in the working tree, which is what
# .gitattributes declares. Reading and writing them the same way is what keeps a
# version bump to the four version lines instead of the whole file.
$resourceEncoding = New-Object System.Text.UnicodeEncoding($false, $true)
$manifestEncoding = New-Object System.Text.UTF8Encoding($false)

function Read-Versions {
    $versions = @{}

    foreach ($path in $resourceFiles) {
        $text = [System.IO.File]::ReadAllText($path, $resourceEncoding)

        $fileComma = [regex]::Match($text, '(?m)^\s*FILEVERSION\s+([\d,]+)\s*$')
        $productComma = [regex]::Match($text, '(?m)^\s*PRODUCTVERSION\s+([\d,]+)\s*$')
        $fileText = [regex]::Match($text, '"FileVersion",\s*"([^"]*)"')
        $productText = [regex]::Match($text, '"ProductVersion",\s*"([^"]*)"')

        if (-not ($fileComma.Success -and $productComma.Success -and $fileText.Success -and $productText.Success)) {
            throw "Could not read all four version values from $path."
        }

        $versions[$path] = [pscustomobject]@{
            FileComma    = $fileComma.Groups[1].Value
            ProductComma = $productComma.Groups[1].Value
            FileText     = $fileText.Groups[1].Value
            ProductText  = $productText.Groups[1].Value
        }
    }

    # Each application identity block, matched from its opening tag: the
    # Common-Controls dependency block also carries a version attribute, and that
    # one must not be touched.
    foreach ($path in $manifestFiles) {
        $manifestText = [System.IO.File]::ReadAllText($path, $manifestEncoding)
        $identity = [regex]::Match($manifestText, '<assemblyIdentity\s+version="([^"]*)"')
        if (-not $identity.Success) {
            throw "Could not read the assembly version from $path."
        }

        $versions[$path] = [pscustomobject]@{ Manifest = $identity.Groups[1].Value }
    }

    return $versions
}

# The four-field form every file stores, so that "1.2.3" and "1.2.3.0" are the
# same version to the comparison below - which is what lets the CI pass its tag
# ("v1.2.3") straight through.
function ConvertTo-NormalizedVersion {
    param([string]$Text)

    $parsed = [regex]::Match($Text, '^v?(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?$')
    if (-not $parsed.Success) {
        throw "Version must look like 1.2.3 or 1.2.3.4, not '$Text'."
    }

    $revision = if ($parsed.Groups[4].Success) { [int]$parsed.Groups[4].Value } else { 0 }
    return "$([int]$parsed.Groups[1].Value).$([int]$parsed.Groups[2].Value).$([int]$parsed.Groups[3].Value).$revision"
}

function Assert-Versions {
    param([hashtable]$Versions, [string]$Expected)

    $reference = ''
    foreach ($path in $resourceFiles) {
        $entry = $Versions[$path]

        if ($entry.FileComma -ne $entry.ProductComma) {
            throw "$path has FILEVERSION $($entry.FileComma) but PRODUCTVERSION $($entry.ProductComma)."
        }

        if ($entry.FileText -ne $entry.ProductText) {
            throw "$path has FileVersion $($entry.FileText) but ProductVersion $($entry.ProductText)."
        }

        $expectedComma = ($entry.FileText -split '\.') -join ','
        if ($entry.FileComma -ne $expectedComma) {
            throw "$path has FILEVERSION $($entry.FileComma) but FileVersion $($entry.FileText)."
        }

        if (-not $reference) {
            $reference = $entry.FileText
        }
        elseif ($entry.FileText -ne $reference) {
            throw "$path reports $($entry.FileText) but $($resourceFiles[0]) reports $reference."
        }
    }

    foreach ($path in $manifestFiles) {
        $manifest = $Versions[$path].Manifest
        if ($manifest -ne $reference) {
            throw "$path reports $manifest but the resource scripts report $reference."
        }
    }

    if ($Expected) {
        $normalized = ConvertTo-NormalizedVersion $Expected
        if ($reference -ne $normalized) {
            throw "The version files report $reference, but $normalized was expected."
        }
    }

    return $reference
}

function Set-Version {
    param([string]$Comma, [string]$Dotted)

    foreach ($path in $resourceFiles) {
        $text = [System.IO.File]::ReadAllText($path, $resourceEncoding)

        # Each pattern is required to match, so a version line that was renamed or
        # removed still fails loudly. "The replacement changed nothing" is not the
        # same thing and is handled below: it means the file already carries the
        # version being written.
        $patterns = @(
            '(?m)^\s*FILEVERSION\s+[\d,]+\s*$',
            '(?m)^\s*PRODUCTVERSION\s+[\d,]+\s*$',
            '"FileVersion",\s*"[^"]*"',
            '"ProductVersion",\s*"[^"]*"'
        )
        foreach ($pattern in $patterns) {
            if ($text -notmatch $pattern) {
                throw "Could not find $pattern in $path."
            }
        }

        $updated = $text `
            -replace '(?m)^(\s*FILEVERSION\s+)[\d,]+', "`${1}$Comma" `
            -replace '(?m)^(\s*PRODUCTVERSION\s+)[\d,]+', "`${1}$Comma" `
            -replace '("FileVersion",\s*")[^"]*(")', "`${1}$Dotted`${2}" `
            -replace '("ProductVersion",\s*")[^"]*(")', "`${1}$Dotted`${2}"

        if ($updated -eq $text) {
            Write-Host "  $([System.IO.Path]::GetFileName($path)) already at $Dotted"
            continue
        }

        [System.IO.File]::WriteAllText($path, $updated, $resourceEncoding)
        Write-Host "  $([System.IO.Path]::GetFileName($path)) -> $Dotted"
    }

    foreach ($path in $manifestFiles) {
        $manifestText = [System.IO.File]::ReadAllText($path, $manifestEncoding)

        if ($manifestText -notmatch '<assemblyIdentity\s+version="[^"]*"') {
            throw "Could not find the assembly version in $path."
        }

        $updatedManifest = $manifestText -replace '(<assemblyIdentity\s+version=")[^"]*(")', "`${1}$Dotted`${2}"

        if ($updatedManifest -eq $manifestText) {
            Write-Host "  $([System.IO.Path]::GetFileName($path)) already at $Dotted"
            continue
        }

        [System.IO.File]::WriteAllText($path, $updatedManifest, $manifestEncoding)
        Write-Host "  $([System.IO.Path]::GetFileName($path)) -> $Dotted"
    }
}

if ($Verify) {
    $current = Assert-Versions -Versions (Read-Versions) -Expected $Version
    Write-Host "Version files are consistent: $current"
    exit 0
}

if (-not $Version) {
    throw "Pass -Version to set the version, or -Verify to check the existing one."
}

$dotted = ConvertTo-NormalizedVersion $Version

Write-Host "Setting the version to $dotted"
Set-Version -Comma ($dotted -replace '\.', ',') -Dotted $dotted

# Read back rather than trusting the replacement, so a pattern that stopped
# matching some file cannot pass unnoticed.
Assert-Versions -Versions (Read-Versions) -Expected $dotted | Out-Null
Write-Host "Version files are consistent."
