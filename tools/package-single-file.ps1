<#
.SYNOPSIS
    Bundles the built application folder into the single-file launcher.

.DESCRIPTION
    Concatenates the launcher executable, every runtime file of the application
    output and a footer describing the payload, and writes the result to

        dist\<Platform>\AudioPlaybackConnectorWinUI-<Platform>-<Deployment>.exe

    Both deployment models are packaged this way, and the file name says which one
    a download is:

        SelfContained       carries the Windows App SDK binaries, so the single
                            file also runs on a machine that has no Windows App
                            Runtime installed.
        FrameworkDependent  carries only Microsoft.WindowsAppRuntime.Bootstrap.dll
                            and uses the Windows App Runtime that is installed on
                            the machine. The file is about ten times smaller, but
                            it needs the Windows App Runtime 2.5.1 runtime.

    Both models share one application output directory, so the payload is
    validated here: a framework-dependent package that still contains the runtime
    means a previous self-contained build was not cleaned. Use
    tools/build-single-file.ps1, which builds and packages one variant at a time.

    The licences are written next to the packaged file, because the file itself
    redistributes Microsoft's Windows App SDK runtime and one of its licence
    conditions is to carry the notices:

        dist\<Platform>\LICENSE.txt                 this project's MIT licence
        dist\<Platform>\THIRD-PARTY-NOTICES.txt     the components, and the full
                                                    Windows App SDK terms and
                                                    notices taken from the NuGet
                                                    packages that were restored

    The notices are assembled before the application is compiled by
    tools\assemble-third-party-notices.ps1 and compiled into the executable as a
    compressed resource the dialog decompresses in memory, and this script only
    copies the uncompressed text here. Nothing is assembled twice, so the copy beside
    the executable, the copy in the repository and the text inside the executable
    cannot drift apart.

.EXAMPLE
    pwsh tools/build-single-file.ps1 -Platform x64 -Deployment FrameworkDependent
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [string]$Configuration = 'Release',
    [ValidateSet('SelfContained', 'FrameworkDependent')]
    [string]$Deployment = 'SelfContained'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $root "$Platform\$Configuration"
$launcherPath = Join-Path $root "$Platform\Launcher\$Configuration\AudioPlaybackConnectorWinUI.Launcher.exe"
# One output directory per platform, so both architectures and both deployment
# models can be packaged side by side without one overwriting the other.
$outDir = Join-Path $root "dist\$Platform"
$outFile = Join-Path $outDir "AudioPlaybackConnectorWinUI-$Platform-$Deployment.exe"
$selfContained = $Deployment -eq 'SelfContained'

if (-not (Test-Path -LiteralPath $appDir)) {
    throw "Application output not found: $appDir. Build the solution first."
}
if (-not (Test-Path -LiteralPath $launcherPath)) {
    throw "Launcher not found: $launcherPath. Build the solution first."
}

# Build artefacts rather than runtime payload, plus the settings file and
# diagnostic log a previous run may have left in the output directory: shipping
# those would hand a fresh installation the build machine's settings.
$excludedExtensions = @('.pdb', '.lib', '.exp', '.winmd', '.ilk')
$excludedNames = @(
    'AudioPlaybackConnectorWinUI.Launcher.exe',
    'AudioPlaybackConnectorWinUI.json',
    'AudioPlaybackConnectorWinUI.log',
    # Redistributed by the Windows App SDK but never used by this application, which
    # hosts no WebView2 control and no RichEditBox. Both are large - WinUIEdit.dll
    # alone is 3.4 MB - and the payload is what the distributed single file carries,
    # so they are left out. See $notRedistributed in
    # tools\assemble-third-party-notices.ps1 for the notices this affects.
    'Microsoft.Web.WebView2.Core.dll',
    'WinUIEdit.dll'
)

$files = Get-ChildItem -LiteralPath $appDir -Recurse -File |
    Where-Object { $excludedExtensions -notcontains $_.Extension } |
    Where-Object { $excludedNames -notcontains $_.Name } |
    Sort-Object FullName

if ($files.Count -eq 0) {
    throw "No payload files found in $appDir."
}

# The deployment model has to be visible in the payload itself, because the two
# differ only in whether the runtime follows the bootstrap library.
$names = $files.Name
if ($names -notcontains 'Microsoft.WindowsAppRuntime.Bootstrap.dll') {
    throw "The payload in $appDir has no Microsoft.WindowsAppRuntime.Bootstrap.dll, so it cannot start the Windows App Runtime."
}
$carriesRuntime = $names -contains 'Microsoft.UI.Xaml.dll'
if ($selfContained -and -not $carriesRuntime) {
    throw "The self-contained payload in $appDir is missing Microsoft.UI.Xaml.dll. Build with -p:WindowsAppSDKSelfContained=true."
}
if (-not $selfContained -and $carriesRuntime) {
    throw "The framework-dependent payload in $appDir still contains the Windows App SDK binaries. Delete $appDir and rebuild."
}

# Payload identity. Derived from the payload contents, so replacing the
# executable with a new build unpacks into a fresh folder while the settings file
# beside the distributed executable stays where it is.
$manifest = ($files | ForEach-Object {
        '{0}|{1}|{2}' -f $_.FullName.Substring($appDir.Length), $_.Length, $_.LastWriteTimeUtc.Ticks
    }) -join "`n"

$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $digest = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($manifest))
} finally {
    $sha.Dispose()
}
$payloadId = [System.BitConverter]::ToUInt64($digest, 0)

# New-Item has no -LiteralPath: its -Path is the only way to create a directory, and
# it does not expand wildcards when creating one.
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$buffer = New-Object byte[] (1MB)
$output = [System.IO.File]::Create($outFile)

try {
    # 1. The launcher itself.
    $launcher = [System.IO.File]::ReadAllBytes($launcherPath)
    $output.Write($launcher, 0, $launcher.Length)

    # 2. The payload entries.
    $payloadOffset = $output.Position

    foreach ($file in $files) {
        $relative = $file.FullName.Substring($appDir.Length).TrimStart('\')
        $nameBytes = [System.Text.Encoding]::Unicode.GetBytes($relative)

        $output.Write([System.BitConverter]::GetBytes([uint32]$nameBytes.Length), 0, 4)
        $output.Write([System.BitConverter]::GetBytes([uint32]$file.Length), 0, 4)
        $output.Write($nameBytes, 0, $nameBytes.Length)

        $input = [System.IO.File]::OpenRead($file.FullName)
        try {
            while (($read = $input.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $output.Write($buffer, 0, $read)
            }
        } finally {
            $input.Dispose()
        }
    }

    $payloadSize = $output.Position - $payloadOffset

    # 3. The footer, which the launcher reads from the end of the file.
    $output.Write([System.BitConverter]::GetBytes([uint64]$payloadOffset), 0, 8)
    $output.Write([System.BitConverter]::GetBytes([uint64]$payloadSize), 0, 8)
    $output.Write([System.BitConverter]::GetBytes([uint64]$payloadId), 0, 8)
    $output.Write([System.Text.Encoding]::ASCII.GetBytes('APCPAY01'), 0, 8)
} finally {
    $output.Dispose()
}

$sizeMb = [math]::Round((Get-Item -LiteralPath $outFile).Length / 1MB, 1)
$model = if ($selfContained) { 'self-contained' } else { 'framework-dependent' }
Write-Host "Packaged $($files.Count) payload files into $outFile ($sizeMb MB, $model)"

# The third-party notices were assembled before the application was compiled
# (tools\build-single-file.ps1 runs tools\assemble-third-party-notices.ps1), so this
# script has nothing left to assemble: it copies the text here. The executable's
# RCDATA (301) resource holds the compressed form of exactly these bytes, so the copy
# and what the dialog shows come from one assembly step, and this step cannot
# silently change what either of them says.
$noticesSource = Join-Path $root 'THIRD-PARTY-NOTICES.txt'
if (-not (Test-Path -LiteralPath $noticesSource)) {
    throw "The assembled third-party notices are missing: $noticesSource. Run tools/build-single-file.ps1, which assembles them before the application is compiled."
}

# The project's own MIT licence. It already carries both copyright notices, and
# the application shows this same file in its licence dialog, so it is copied
# verbatim.
#
# -Encoding utf8 on the read, not just here: the licence is UTF-8, and Windows
# PowerShell 5.1 defaults Get-Content to the ANSI code page. The script is reached
# through build-single-file.ps1, so the host is whichever shell the developer
# launched.
$ownLicence = (Get-Content -LiteralPath (Join-Path $root 'LICENSE') -Raw -Encoding utf8) -replace "`r?`n", "`r`n"
[System.IO.File]::WriteAllText((Join-Path $outDir 'LICENSE.txt'), $ownLicence, (New-Object System.Text.UTF8Encoding($true)))

# The bytes, not the text: a round trip through a string could re-encode or
# normalise something, and the executable embeds the compressed form of exactly
# these bytes.
Copy-Item -LiteralPath $noticesSource -Destination (Join-Path $outDir 'THIRD-PARTY-NOTICES.txt') -Force

Write-Host "Wrote LICENSE.txt and THIRD-PARTY-NOTICES.txt beside $([System.IO.Path]::GetFileName($outFile))"
