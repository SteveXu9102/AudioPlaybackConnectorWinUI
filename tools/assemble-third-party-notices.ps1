<#
.SYNOPSIS
    Assembles the complete third-party notices the application embeds and ships.

.DESCRIPTION
    The component list in tools\THIRD-PARTY-NOTICES.txt is the one editable source.
    This script appends the licence and notice text of every redistributed NuGet
    package to it and writes two files to the repository root:

        THIRD-PARTY-NOTICES.txt             the complete text, as it is shipped
        THIRD-PARTY-NOTICES.compressed      the same bytes, MSZIP compressed

    The complete text is written before the application is compiled, so the resource
    script embeds it as RCDATA (301) - compressed, because the text is about 889 KB
    and the compressed form about 92 KB - and tools\package-single-file.ps1 copies
    the uncompressed file beside each packaged executable. The dialog decompresses
    the resource in memory, so the text it shows is the text that was shipped and
    nothing is read from disk.

    Which packages have to be noticed is read from obj\project.assets.json rather
    than from the project file, because the payload also carries binaries from
    packages the project references only transitively, and the assembled text has
    to name the versions the restore of this platform actually resolved.

    The compressed file is produced here rather than by a separate tool so that the
    two outputs cannot be built from different text: the compressor is handed the
    bytes of the file that was just written.

.PARAMETER Platform
    The restore target whose packages are assembled. Both architectures resolve the
    same package set; the parameter selects the target in the assets file.

.EXAMPLE
    pwsh tools/assemble-third-party-notices.ps1 -Platform x64
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$noticesBase = Join-Path $PSScriptRoot 'THIRD-PARTY-NOTICES.txt'
$noticesTarget = Join-Path $root 'THIRD-PARTY-NOTICES.txt'
$noticesCompressed = Join-Path $root 'THIRD-PARTY-NOTICES.compressed'

if (-not (Test-Path -LiteralPath $noticesBase)) {
    throw "The canonical third-party notices are missing: $noticesBase"
}

function Get-NuGetPackageDir {
    param([string]$Id, [string]$Version)

    $roots = @()
    if ($env:NUGET_PACKAGES) { $roots += $env:NUGET_PACKAGES }
    $roots += (Join-Path $env:USERPROFILE '.nuget\packages')

    foreach ($packageRoot in $roots) {
        $candidate = Join-Path $packageRoot (Join-Path $Id.ToLowerInvariant() $Version)
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

$assetsPath = Join-Path $root 'obj\project.assets.json'
if (-not (Test-Path -LiteralPath $assetsPath)) {
    throw "Restore assets not found: $assetsPath. Restore the solution first."
}
$assets = Get-Content -LiteralPath $assetsPath -Raw | ConvertFrom-Json

$packageIds = New-Object System.Collections.Generic.SortedSet[string]
foreach ($target in $assets.targets.PSObject.Properties) {
    if ($target.Name -notlike "*$Platform*") { continue }
    foreach ($package in $target.Value.PSObject.Properties) {
        [void]$packageIds.Add($package.Name)
    }
}
if ($packageIds.Count -eq 0) {
    throw "The restore assets in $assetsPath have no target for $Platform."
}

# Packages whose binaries the payload does not carry, so their notices must not be
# appended either: the assembled file describes exactly what is distributed.
# Microsoft.Web.WebView2 is referenced transitively by the WinUI package, but its
# only redistributable that would land next to the executable -
# Microsoft.Web.WebView2.Core.dll - is pruned by $excludedNames in
# tools\package-single-file.ps1.
$notRedistributed = @('Microsoft.Web.WebView2')

$appended = @()
foreach ($packageId in $packageIds) {
    $parts = $packageId -split '/', 2
    $id = $parts[0]
    $version = $parts[1]

    if ($notRedistributed -contains $id) { continue }

    $packageDir = Get-NuGetPackageDir -Id $id -Version $version
    if (-not $packageDir) {
        Write-Warning "Package $packageId is not in the NuGet package folders; nothing was appended for it."
        continue
    }

    # The names NuGet packages actually use. Every match is appended, not the first:
    # a package can ship its own licence *and* a separate file for the components it
    # incorporates, and picking one would drop the other. ThirdPartyNotices.txt is
    # Microsoft.Windows.ImplementationLibrary's file for the components WIL
    # incorporates (Libc++ and Catch2), and sdk_license.txt is
    # Microsoft.Windows.SDK.BuildTools.MSIX's.
    $found = @()
    foreach ($candidate in @('license.txt', 'LICENSE.txt', 'LICENSE', 'NOTICE.txt', 'ThirdPartyNotices.txt', 'sdk_license.txt')) {
        $path = Join-Path $packageDir $candidate
        if (Test-Path -LiteralPath $path) { $found += $path }
    }

    foreach ($path in $found) {
        $appended += @{
            Path  = $path
            Title = "$id $version - $((Split-Path -Leaf $path))"
        }
    }

    if ($found.Count -eq 0) {
        Write-Warning "Package $packageId ships none of the known licence or notice names; nothing was appended for it. Check whether it redistributes anything."
    }
}

# The package whose binaries are demonstrably in the payload. A missing notice for it
# is a build error rather than something to warn about: the file assembled here is
# what the program compiles in, so it cannot be allowed to ship without those terms.
foreach ($required in @('Microsoft.WindowsAppSDK.Runtime')) {
    if (-not ($appended | Where-Object { $_.Title -like "$required *" })) {
        throw "No licence or notice text was found for $required, whose binaries are redistributed in the payload."
    }
}

foreach ($entry in @(@{ Path = $noticesBase }) + $appended) {
    if (-not (Test-Path -LiteralPath $entry.Path)) {
        throw "Required notice file not found: $($entry.Path)"
    }
}

# The component description, then the terms of the components that are packaged.
# Every read is explicitly UTF-8: the Windows App SDK's terms carry non-ASCII
# punctuation, and Windows PowerShell 5.1 defaults Get-Content to the ANSI code
# page - which would turn the legally required text into mojibake. The script is
# reached from tools\build-single-file.ps1, so the host is whichever shell the
# developer launched.
$notices = New-Object System.Text.StringBuilder
[void]$notices.AppendLine(((Get-Content -LiteralPath $noticesBase -Raw -Encoding utf8) -replace "`r?`n", "`r`n"))
foreach ($entry in $appended) {
    [void]$notices.AppendLine()
    [void]$notices.AppendLine(('=' * 78))
    [void]$notices.AppendLine($entry.Title)
    [void]$notices.AppendLine(('=' * 78))
    [void]$notices.AppendLine()
    [void]$notices.AppendLine(((Get-Content -LiteralPath $entry.Path -Raw -Encoding utf8) -replace "`r?`n", "`r`n"))
}

# A byte order mark, because the text is UTF-8 and a reader that assumes the ANSI
# code page would otherwise mis-decode it.
[System.IO.File]::WriteAllText($noticesTarget, $notices.ToString(), (New-Object System.Text.UTF8Encoding($true)))

# The Compression API in cabinet.dll, which is the documented way to read an MSZIP
# stream. MSZIP is used because it measured smallest of the algorithms the API
# offers on this text - about 92 KB against 141 KB for LZMS, 152 KB for
# XPRESS_HUFF and 453 KB for XPRESS - and it decompresses in about 2 ms, which the
# dialog can spend when it opens. The reader in LicenseDialogWindow.xaml.cpp uses
# the same API and reads the algorithm id from the header below, so the build can
# change the format without a change there.
if (-not ('Notices.Compression' -as [type])) {
    Add-Type -Namespace Notices -Name Compression -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("cabinet.dll", SetLastError = true)]
public static extern bool CreateCompressor(uint Algorithm, System.IntPtr AllocationRoutines, out System.IntPtr CompressorHandle);
[System.Runtime.InteropServices.DllImport("cabinet.dll", SetLastError = true)]
public static extern bool Compress(System.IntPtr CompressorHandle, byte[] UncompressedData, System.UIntPtr UncompressedDataSize,
    byte[] CompressedBuffer, System.UIntPtr CompressedBufferSize, out System.UIntPtr CompressedDataSize);
[System.Runtime.InteropServices.DllImport("cabinet.dll", SetLastError = true)]
public static extern bool CloseCompressor(System.IntPtr CompressorHandle);
[System.Runtime.InteropServices.DllImport("cabinet.dll", SetLastError = true)]
public static extern bool CreateDecompressor(uint Algorithm, System.IntPtr AllocationRoutines, out System.IntPtr DecompressorHandle);
[System.Runtime.InteropServices.DllImport("cabinet.dll", SetLastError = true)]
public static extern bool Decompress(System.IntPtr DecompressorHandle, byte[] CompressedData, System.UIntPtr CompressedDataSize,
    byte[] UncompressedBuffer, System.UIntPtr UncompressedBufferSize, out System.UIntPtr UncompressedDataSize);
[System.Runtime.InteropServices.DllImport("cabinet.dll", SetLastError = true)]
public static extern bool CloseDecompressor(System.IntPtr DecompressorHandle);
'@
}

$kAlgorithmMszip = [uint32]2   # COMPRESS_ALGORITHM_MSZIP
$kHeaderBytes = 12             # 'APCN', then the plain size, then the algorithm id

# The bytes of the file that was just written, not of the string above: the dialog
# has to be able to reproduce that file exactly, so the compressor is handed exactly
# what a reader of the file would get, byte order mark included.
$plainBytes = [System.IO.File]::ReadAllBytes($noticesTarget)

$compressor = [System.IntPtr]::Zero
if (-not [Notices.Compression]::CreateCompressor($kAlgorithmMszip, [System.IntPtr]::Zero, [ref]$compressor)) {
    throw "CreateCompressor failed with error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())."
}

try {
    $capacity = $plainBytes.Length + [int]($plainBytes.Length / 2) + 4096
    $buffer = New-Object byte[] $capacity
    $produced = [System.UIntPtr]::Zero
    if (-not [Notices.Compression]::Compress($compressor, $plainBytes, [System.UIntPtr]$plainBytes.Length, $buffer, [System.UIntPtr]$capacity, [ref]$produced)) {
        throw "Compress failed with error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())."
    }
    $compressedSize = [int]$produced.ToUInt64()
} finally {
    [void][Notices.Compression]::CloseCompressor($compressor)
}

$output = New-Object System.IO.MemoryStream
try {
    $output.Write([System.Text.Encoding]::ASCII.GetBytes('APCN'), 0, 4)
    $output.Write([System.BitConverter]::GetBytes([uint32]$plainBytes.Length), 0, 4)
    $output.Write([System.BitConverter]::GetBytes($kAlgorithmMszip), 0, 4)
    $output.Write($buffer, 0, $compressedSize)
    [System.IO.File]::WriteAllBytes($noticesCompressed, $output.ToArray())
} finally {
    $output.Dispose()
}

# Proves the two outputs agree before either is used, so a compressor that changed
# its mind is a failed build rather than a dialog that shows garbage.
$verifyCompressor = [System.IntPtr]::Zero
if (-not [Notices.Compression]::CreateDecompressor($kAlgorithmMszip, [System.IntPtr]::Zero, [ref]$verifyCompressor)) {
    throw "CreateDecompressor failed with error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())."
}
try {
    $restored = New-Object byte[] $plainBytes.Length
    $restoredSize = [System.UIntPtr]::Zero
    $ok = [Notices.Compression]::Decompress($verifyCompressor, $buffer, [System.UIntPtr]$compressedSize, $restored, [System.UIntPtr]$plainBytes.Length, [ref]$restoredSize)
    if (-not $ok) {
        throw "The compressed notices did not decompress with error $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())."
    }
} finally {
    [void][Notices.Compression]::CloseDecompressor($verifyCompressor)
}

function Get-Sha256([byte[]]$Data) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return [System.BitConverter]::ToString($sha.ComputeHash($Data)).Replace('-', '') } finally { $sha.Dispose() }
}

$plainHash = Get-Sha256 $plainBytes
$restoredHash = Get-Sha256 $restored
if ($restoredHash -ne $plainHash) {
    throw "The compressed notices do not decompress to $noticesTarget; refusing to continue."
}

$characters = $notices.Length
$sizeKb = [math]::Round((Get-Item -LiteralPath $noticesTarget).Length / 1KB, 1)
$compressedKb = [math]::Round((Get-Item -LiteralPath $noticesCompressed).Length / 1KB, 1)
$ratio = [math]::Round(100.0 * (Get-Item -LiteralPath $noticesCompressed).Length / (Get-Item -LiteralPath $noticesTarget).Length, 1)
Write-Host "notices   : $noticesTarget ($($appended.Count) package files appended, $characters characters, $sizeKb KB, from $noticesBase)"
Write-Host "compressed: $noticesCompressed ($compressedKb KB, $ratio% of the text, MSZIP, sha256 $plainHash)"
