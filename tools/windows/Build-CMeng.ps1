[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",

    [ValidateSet("quiet", "minimal", "normal", "detailed", "diagnostic")]
    [string]$MSBuildVerbosity = "normal",

    [string]$BuildDirectory = "",

    [string]$ShortcutName = "Claymore.lnk",

    [switch]$SkipGitPull,

    [switch]$SkipDesktopShortcut
)

<#
.SYNOPSIS
Builds the cmeng/Claymore engine on Windows.

.DESCRIPTION
This script can:
 - pull the latest repository changes;
 - hard reset and retry the pull if the first pull fails and you confirm it;
 - initialize/update git submodules;
 - ensure the required bgfx Windows artifacts exist and build them if missing;
 - configure/build the Visual Studio CMake project;
 - create a desktop shortcut to the resulting Claymore.exe.

bgfx's current official Windows build documentation still generates `vs2022`
projects, and this repository's CMake files are hard-coded to
`external/bgfx/.build/win64_vs2022`, so bgfx is intentionally built through
that compatible path.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\tools\windows\Build-CMeng.ps1

.EXAMPLE
powershell -ExecutionPolicy Bypass -File .\tools\windows\Build-CMeng.ps1 -Configuration RelWithDebInfo
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ScriptRoot) -and -not [string]::IsNullOrWhiteSpace($PSCommandPath)) {
    $ScriptRoot = Split-Path -Path $PSCommandPath -Parent
}
if ([string]::IsNullOrWhiteSpace($ScriptRoot)) {
    throw "Unable to resolve the script root directory."
}

$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $ScriptRoot "..\.."))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $RepoRoot "build"
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$DesktopDirectory = [Environment]::GetFolderPath("DesktopDirectory")
$BgfxRoot = Join-Path $RepoRoot "external\bgfx"
$BxRoot = Join-Path $RepoRoot "external\bx"
$BimgRoot = Join-Path $RepoRoot "external\bimg"
$BgfxProjectFlavor = "vs2022"
$BgfxSolution = Join-Path $BgfxRoot ".build\projects\$BgfxProjectFlavor\bgfx.sln"
$BgfxOutputDirectory = Join-Path $BgfxRoot ".build\win64_$BgfxProjectFlavor\bin"
$BgfxBuildConfig = if ($Configuration -eq "Debug") { "Debug" } else { "Release" }
$EngineExecutable = Join-Path $BuildDirectory "$Configuration\Claymore.exe"

function Set-BuildProgress {
    param(
        [Parameter(Mandatory = $true)]
        [int]$Percent,

        [Parameter(Mandatory = $true)]
        [string]$Status
    )

    Write-Progress -Id 1 -Activity "Building cmeng" -Status $Status -PercentComplete $Percent
}

function Clear-BuildProgress {
    Write-Progress -Id 1 -Activity "Building cmeng" -Completed
}

function Write-Section {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Title
    )

    Write-Host ""
    Write-Host "=== $Title ===" -ForegroundColor Cyan
}

function Test-CommandAvailable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    return $null -ne (Get-Command -Name $Name -ErrorAction SilentlyContinue)
}

function Assert-CommandAvailable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if (-not (Test-CommandAvailable -Name $Name)) {
        throw "Required command was not found on PATH: $Name"
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$ArgumentList = @(),

        [string]$WorkingDirectory = $RepoRoot,

        [switch]$AllowFailure
    )

    $displayCommand = @($FilePath) + $ArgumentList
    Write-Host ("> " + ($displayCommand -join " "))

    Push-Location $WorkingDirectory
    try {
        & $FilePath @ArgumentList
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "Command failed with exit code ${exitCode}: $($displayCommand -join ' ')"
    }

    return $exitCode
}

function Get-VSWherePath {
    $installerRoot = ${env:ProgramFiles(x86)}
    if ([string]::IsNullOrWhiteSpace($installerRoot)) {
        return $null
    }

    $vswhere = Join-Path $installerRoot "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        return $vswhere
    }

    return $null
}

function Get-VisualStudioInstances {
    $vswhere = Get-VSWherePath
    if (-not $vswhere) {
        return @()
    }

    $jsonLines = & $vswhere -products * -requires Microsoft.Component.MSBuild -format json
    if ($LASTEXITCODE -ne 0 -or -not $jsonLines) {
        return @()
    }

    $jsonText = ($jsonLines -join [Environment]::NewLine).Trim()
    if ([string]::IsNullOrWhiteSpace($jsonText)) {
        return @()
    }

    $instances = $jsonText | ConvertFrom-Json
    return @($instances | Sort-Object { [version]$_.installationVersion } -Descending)
}

function Get-VisualStudioInstance {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Instances,

        [Parameter(Mandatory = $true)]
        [int]$MajorVersion
    )

    foreach ($instance in $Instances) {
        $version = [version]$instance.installationVersion
        if ($version.Major -eq $MajorVersion) {
            return $instance
        }
    }

    return $null
}

function Get-MSBuildPath {
    param(
        [Parameter(Mandatory = $true)]
        [object]$VisualStudioInstance
    )

    foreach ($candidate in @(
        (Join-Path $VisualStudioInstance.installationPath "MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path $VisualStudioInstance.installationPath "MSBuild\17.0\Bin\MSBuild.exe"),
        (Join-Path $VisualStudioInstance.installationPath "MSBuild\18.0\Bin\MSBuild.exe")
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "MSBuild.exe was not found under Visual Studio instance: $($VisualStudioInstance.installationPath)"
}

function Test-CMakeGeneratorAvailable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$GeneratorName
    )

    $helpText = & cmake --help | Out-String
    return $helpText -match [regex]::Escape($GeneratorName)
}

function Get-PreferredCMakeGenerator {
    param(
        [object]$VS2026,
        [object]$VS2022
    )

    if ($VS2026 -and (Test-CMakeGeneratorAvailable -GeneratorName "Visual Studio 18 2026")) {
        return "Visual Studio 18 2026"
    }

    if ($VS2022 -and (Test-CMakeGeneratorAvailable -GeneratorName "Visual Studio 17 2022")) {
        return "Visual Studio 17 2022"
    }

    throw "No supported Visual Studio CMake generator was found. Install a compatible Visual Studio workload and ensure `cmake --help` lists the generator."
}

function Get-CMakeCacheEntry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CachePath,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
        return $null
    }

    $match = Select-String -Path $CachePath -Pattern ("^{0}:[^=]*=(.*)$" -f [regex]::Escape($Name)) | Select-Object -First 1
    if ($match) {
        return $match.Matches[0].Groups[1].Value
    }

    return $null
}

function Test-CMakeFreshRequired {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildPath,

        [Parameter(Mandatory = $true)]
        [string]$GeneratorName
    )

    $cachePath = Join-Path $BuildPath "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        return $false
    }

    $cachedGenerator = Get-CMakeCacheEntry -CachePath $cachePath -Name "CMAKE_GENERATOR"
    $cachedPlatform = Get-CMakeCacheEntry -CachePath $cachePath -Name "CMAKE_GENERATOR_PLATFORM"
    $cachedInstance = Get-CMakeCacheEntry -CachePath $cachePath -Name "CMAKE_GENERATOR_INSTANCE"

    if ($cachedGenerator -and $cachedGenerator -ne $GeneratorName) {
        return $true
    }

    if ($cachedPlatform -and $cachedPlatform -ne "x64") {
        return $true
    }

    if ($cachedInstance -and -not (Test-Path -LiteralPath $cachedInstance)) {
        return $true
    }

    return $false
}

function Get-CurrentBranch {
    Push-Location $RepoRoot
    try {
        $branch = & git branch --show-current 2>$null
        if ($LASTEXITCODE -eq 0) {
            return $branch.Trim()
        }
    }
    finally {
        Pop-Location
    }

    return $null
}

function Get-UpstreamRef {
    Push-Location $RepoRoot
    try {
        $upstream = & git rev-parse --abbrev-ref --symbolic-full-name "@{u}" 2>$null
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($upstream)) {
            return $upstream.Trim()
        }
    }
    finally {
        Pop-Location
    }

    return $null
}

function Get-OriginBranchRef {
    $branch = Get-CurrentBranch
    if (-not [string]::IsNullOrWhiteSpace($branch)) {
        return "origin/$branch"
    }

    return $null
}

function Invoke-GitPullLatest {
    $upstream = Get-UpstreamRef
    if ($upstream) {
        return Invoke-Native -FilePath "git" -ArgumentList @("pull", "--ff-only") -WorkingDirectory $RepoRoot -AllowFailure
    }

    $originBranch = Get-OriginBranchRef
    if ([string]::IsNullOrWhiteSpace($originBranch)) {
        throw "Unable to determine the current branch for git pull."
    }

    return Invoke-Native -FilePath "git" -ArgumentList @("pull", "--ff-only", "origin", $originBranch.Replace("origin/", "")) -WorkingDirectory $RepoRoot -AllowFailure
}

function Confirm-HardReset {
    while ($true) {
        $response = Read-Host "Git pull failed and a hard reset will discard local changes. Are you sure? (Y/N)"
        switch -Regex ($response.Trim()) {
            "^(?i)y(?:es)?$" { return $true }
            "^(?i)n(?:o)?$" { return $false }
            default { Write-Host "Please answer Y or N." -ForegroundColor Yellow }
        }
    }
}

function Sync-Submodules {
    param(
        [switch]$Force
    )

    Invoke-Native -FilePath "git" -ArgumentList @("submodule", "sync", "--recursive") -WorkingDirectory $RepoRoot | Out-Null

    $args = @("submodule", "update", "--init", "--recursive")
    if ($Force) {
        $args += "--force"
    }

    Invoke-Native -FilePath "git" -ArgumentList $args -WorkingDirectory $RepoRoot | Out-Null
}

function Reset-RepoAndSubmodulesHard {
    $upstream = Get-UpstreamRef
    $originBranch = Get-OriginBranchRef

    if ($upstream) {
        Invoke-Native -FilePath "git" -ArgumentList @("reset", "--hard", $upstream) -WorkingDirectory $RepoRoot | Out-Null
    }
    elseif ($originBranch) {
        Invoke-Native -FilePath "git" -ArgumentList @("reset", "--hard", $originBranch) -WorkingDirectory $RepoRoot | Out-Null
    }
    else {
        Invoke-Native -FilePath "git" -ArgumentList @("reset", "--hard", "HEAD") -WorkingDirectory $RepoRoot | Out-Null
    }

    Sync-Submodules -Force
}

function Update-Repository {
    if ($SkipGitPull) {
        Write-Host "Skipping git pull because -SkipGitPull was provided."
    }

    if (-not $SkipGitPull) {
        Invoke-Native -FilePath "git" -ArgumentList @("fetch", "--all", "--prune") -WorkingDirectory $RepoRoot | Out-Null

        $pullExitCode = Invoke-GitPullLatest
        if ($pullExitCode -ne 0) {
            if (-not (Confirm-HardReset)) {
                throw "Aborted by user after git pull failure."
            }

            Invoke-Native -FilePath "git" -ArgumentList @("fetch", "--all", "--prune") -WorkingDirectory $RepoRoot | Out-Null
            Reset-RepoAndSubmodulesHard
            Write-Host "Repository reset to the latest fetched upstream revision and submodules were force-aligned." -ForegroundColor Yellow
        }
    }

    Sync-Submodules
}

function Ensure-BgfxSource {
    if (-not (Test-Path -LiteralPath $BgfxRoot -PathType Container)) {
        Write-Host "external/bgfx is missing. Cloning the official bgfx repository..."
        Invoke-Native -FilePath "git" -ArgumentList @("clone", "https://github.com/bkaradzic/bgfx.git", $BgfxRoot) -WorkingDirectory $RepoRoot | Out-Null
    }

    if (-not (Test-Path -LiteralPath $BxRoot -PathType Container)) {
        throw "Required sibling dependency is missing: $BxRoot"
    }

    if (-not (Test-Path -LiteralPath $BimgRoot -PathType Container)) {
        throw "Required sibling dependency is missing: $BimgRoot"
    }
}

function Ensure-BgfxProjects {
    $genie = Join-Path $BxRoot "tools\bin\windows\genie.exe"
    if (-not (Test-Path -LiteralPath $genie -PathType Leaf)) {
        throw "bgfx project generator was not found: $genie"
    }

    if (-not (Test-Path -LiteralPath $BgfxSolution -PathType Leaf)) {
        Write-Host "Generating bgfx Visual Studio projects..."
        Invoke-Native -FilePath $genie -ArgumentList @("--with-tools", $BgfxProjectFlavor) -WorkingDirectory $BgfxRoot | Out-Null
    }
}

function Get-BgfxExpectedArtifacts {
    $suffix = $BgfxBuildConfig
    return @(
        (Join-Path $BgfxOutputDirectory "bgfx$suffix.lib"),
        (Join-Path $BgfxOutputDirectory "bimg$suffix.lib"),
        (Join-Path $BgfxOutputDirectory "bx$suffix.lib"),
        (Join-Path $BgfxOutputDirectory "shaderc$suffix.exe")
    )
}

function Test-BgfxArtifactsPresent {
    foreach ($path in (Get-BgfxExpectedArtifacts)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            return $false
        }
    }

    return $true
}

function Build-BgfxIfNeeded {
    param(
        [Parameter(Mandatory = $true)]
        [object]$VS2026,

        [Parameter(Mandatory = $true)]
        [object]$VS2022
    )

    Ensure-BgfxSource
    Ensure-BgfxProjects

    if (Test-BgfxArtifactsPresent) {
        Write-Host "bgfx artifacts for $BgfxBuildConfig/x64 are already present."
        return
    }

    $builderInstance = if ($VS2022) { $VS2022 } elseif ($VS2026) { $VS2026 } else { $null }
    if (-not $builderInstance) {
        throw "A Visual Studio installation with MSBuild is required to build bgfx."
    }

    $msbuild = Get-MSBuildPath -VisualStudioInstance $builderInstance
    $targets = "bgfx;bimg;bx;shaderc"

    Write-Host "Building missing bgfx artifacts for $BgfxBuildConfig/x64..."
    Clear-BuildProgress
    Invoke-Native -FilePath $msbuild -ArgumentList @(
        $BgfxSolution,
        "/m",
        "/nologo",
        "/verbosity:$MSBuildVerbosity",
        "/consoleloggerparameters:Summary",
        "/p:Configuration=$BgfxBuildConfig",
        "/p:Platform=x64",
        "/t:$targets"
    ) -WorkingDirectory $BgfxRoot | Out-Null

    if (-not (Test-BgfxArtifactsPresent)) {
        throw "bgfx build completed, but one or more expected artifacts are still missing from $BgfxOutputDirectory"
    }
}

function Configure-CMakeProject {
    param(
        [Parameter(Mandatory = $true)]
        [string]$GeneratorName
    )

    New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null

    $cmakeArgs = @("-S", $RepoRoot, "-B", $BuildDirectory, "-G", $GeneratorName, "-A", "x64")
    if (Test-CMakeFreshRequired -BuildPath $BuildDirectory -GeneratorName $GeneratorName) {
        Write-Host "Refreshing the CMake build directory because the cached generator state is stale or incompatible."
        $cmakeArgs = @("--fresh") + $cmakeArgs
    }

    Invoke-Native -FilePath "cmake" -ArgumentList $cmakeArgs -WorkingDirectory $RepoRoot | Out-Null
}

function Build-Engine {
    Clear-BuildProgress
    Invoke-Native -FilePath "cmake" -ArgumentList @(
        "--build", $BuildDirectory,
        "--config", $Configuration,
        "--target", "ClaymoreEditor.exe",
        "--",
        "/m",
        "/nologo",
        "/verbosity:$MSBuildVerbosity",
        "/consoleloggerparameters:Summary"
    ) -WorkingDirectory $RepoRoot | Out-Null

    if (-not (Test-Path -LiteralPath $EngineExecutable -PathType Leaf)) {
        throw "Expected executable was not produced: $EngineExecutable"
    }
}

function Ensure-DesktopShortcut {
    if ($SkipDesktopShortcut) {
        Write-Host "Skipping desktop shortcut because -SkipDesktopShortcut was provided."
        return $null
    }

    if ([string]::IsNullOrWhiteSpace($DesktopDirectory)) {
        throw "The current user's desktop directory could not be resolved."
    }

    if (-not (Test-Path -LiteralPath $EngineExecutable -PathType Leaf)) {
        throw "Cannot create a shortcut because the executable does not exist: $EngineExecutable"
    }

    if (-not $ShortcutName.EndsWith(".lnk", [System.StringComparison]::OrdinalIgnoreCase)) {
        $ShortcutName = "$ShortcutName.lnk"
    }

    $shortcutPath = Join-Path $DesktopDirectory $ShortcutName
    $wshShell = New-Object -ComObject WScript.Shell
    $shortcut = $wshShell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = $EngineExecutable
    $shortcut.WorkingDirectory = Split-Path -Path $EngineExecutable -Parent
    $shortcut.IconLocation = "$EngineExecutable,0"
    $shortcut.Description = "Launch Claymore built from $RepoRoot"
    $shortcut.Save()

    return $shortcutPath
}

try {
    Assert-CommandAvailable -Name "git"
    Assert-CommandAvailable -Name "cmake"

    $vsInstances = Get-VisualStudioInstances
    $vs2026 = Get-VisualStudioInstance -Instances $vsInstances -MajorVersion 18
    $vs2022 = Get-VisualStudioInstance -Instances $vsInstances -MajorVersion 17
    $cmakeGenerator = Get-PreferredCMakeGenerator -VS2026 $vs2026 -VS2022 $vs2022

    Write-Section -Title "Environment"
    Write-Host "Repository root : $RepoRoot"
    Write-Host "Build directory : $BuildDirectory"
    Write-Host "Configuration   : $Configuration"
    Write-Host "Verbosity       : $MSBuildVerbosity"
    Write-Host "CMake generator : $cmakeGenerator"
    if ($vs2026) {
        Write-Host "VS2026 detected : $($vs2026.installationPath)"
    }
    if ($vs2022) {
        Write-Host "VS2022 detected : $($vs2022.installationPath)"
    }
    if ($cmakeGenerator -eq "Visual Studio 17 2022" -and $vs2026) {
        Write-Host "Using the VS2022 CMake path for compatibility: current bgfx docs and this repo's CMake wiring still target vs2022/win64_vs2022." -ForegroundColor Yellow
    }

    Set-BuildProgress -Percent 5 -Status "Updating repository"
    Write-Section -Title "Git Update"
    Update-Repository

    Set-BuildProgress -Percent 30 -Status "Ensuring bgfx artifacts"
    Write-Section -Title "bgfx"
    Build-BgfxIfNeeded -VS2026 $vs2026 -VS2022 $vs2022

    Set-BuildProgress -Percent 55 -Status "Configuring Visual Studio CMake project"
    Write-Section -Title "CMake Configure"
    Configure-CMakeProject -GeneratorName $cmakeGenerator

    Set-BuildProgress -Percent 80 -Status "Building Claymore.exe"
    Write-Section -Title "Project Build"
    Build-Engine

    Set-BuildProgress -Percent 95 -Status "Creating desktop shortcut"
    Write-Section -Title "Desktop Shortcut"
    $shortcutPath = Ensure-DesktopShortcut

    Set-BuildProgress -Percent 100 -Status "Complete"
    Write-Progress -Id 1 -Activity "Building cmeng" -Completed

    Write-Section -Title "Done"
    Write-Host "Executable : $EngineExecutable" -ForegroundColor Green
    if ($shortcutPath) {
        Write-Host "Shortcut   : $shortcutPath" -ForegroundColor Green
    }
}
catch {
    Write-Progress -Id 1 -Activity "Building cmeng" -Completed
    throw
}
