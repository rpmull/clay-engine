param(
    [Parameter(Mandatory = $true)]
    [string]$PublishDir,

    [string]$RuntimeVersion = "10.0.5",

    [string]$SourceDotnetRoot = "$env:ProgramFiles\dotnet"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $PublishDir)) {
    New-Item -ItemType Directory -Force -Path $PublishDir | Out-Null
}

$resolvedPublishDir = (Resolve-Path -LiteralPath $PublishDir).Path
$resolvedDotnetRoot = (Resolve-Path -LiteralPath $SourceDotnetRoot).Path

$dotnetExeSource = Join-Path $resolvedDotnetRoot "dotnet.exe"
$hostFxrSource = Join-Path $resolvedDotnetRoot "host\fxr\$RuntimeVersion"
$sharedRuntimeSource = Join-Path $resolvedDotnetRoot "shared\Microsoft.NETCore.App\$RuntimeVersion"

if (-not (Test-Path -LiteralPath $dotnetExeSource)) {
    throw "dotnet.exe was not found under '$resolvedDotnetRoot'."
}

if (-not (Test-Path -LiteralPath $hostFxrSource)) {
    throw "hostfxr runtime '$RuntimeVersion' was not found under '$resolvedDotnetRoot\\host\\fxr'."
}

if (-not (Test-Path -LiteralPath $sharedRuntimeSource)) {
    throw "Microsoft.NETCore.App runtime '$RuntimeVersion' was not found under '$resolvedDotnetRoot\\shared\\Microsoft.NETCore.App'."
}

$bundledDotnetDir = Join-Path $resolvedPublishDir "dotnet"
if (Test-Path -LiteralPath $bundledDotnetDir) {
    Remove-Item -LiteralPath $bundledDotnetDir -Recurse -Force
}

$hostFxrTargetParent = Join-Path $bundledDotnetDir "host\fxr"
$sharedRuntimeTargetParent = Join-Path $bundledDotnetDir "shared\Microsoft.NETCore.App"
New-Item -ItemType Directory -Force -Path $hostFxrTargetParent | Out-Null
New-Item -ItemType Directory -Force -Path $sharedRuntimeTargetParent | Out-Null

Copy-Item -LiteralPath $dotnetExeSource -Destination (Join-Path $bundledDotnetDir "dotnet.exe") -Force
Copy-Item -LiteralPath $hostFxrSource -Destination $hostFxrTargetParent -Recurse -Force
Copy-Item -LiteralPath $sharedRuntimeSource -Destination $sharedRuntimeTargetParent -Recurse -Force

$legacyRuntimeFiles = @(
    "hostfxr.dll",
    "hostpolicy.dll",
    "coreclr.dll",
    "clrjit.dll",
    "clrgc.dll",
    "clretwrc.dll",
    "mscordbi.dll",
    "createdump.exe"
)

foreach ($fileName in $legacyRuntimeFiles) {
    $candidate = Join-Path $resolvedPublishDir $fileName
    if (Test-Path -LiteralPath $candidate) {
        Remove-Item -LiteralPath $candidate -Force
    }
}

Get-ChildItem -LiteralPath $resolvedPublishDir -Filter "mscordaccore*.dll" -File -ErrorAction SilentlyContinue |
    Remove-Item -Force

Write-Host "[stage_private_dotnet_runtime] Staged private .NET runtime '$RuntimeVersion' into '$bundledDotnetDir'."
