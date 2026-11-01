param(
    [string]$BuildType = "Development",
    [string]$CommitMessage = "",
    [switch]$SkipCommit = $false,
    [switch]$SkipTests = $false
)

if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
    # Make commit message mandatory if SkipCommit is not set
    if (-not $SkipCommit) {
        throw "CommitMessage parameter is required unless SkipCommit is specified."
    }
}

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

# 1. Read manual semantic version from VERSION.txt
$VersionFile = Join-Path $ProjectRoot "VERSION.txt"
if (-not (Test-Path $VersionFile)) {
    throw "VERSION.txt not found in $ProjectRoot"
}
$ManualVersion = (Get-Content $VersionFile -Raw).Trim()

# 2. Generate Unique Build ID
$UtcNow = (Get-Date).ToUniversalTime()
$TimestampCompact = $UtcNow.ToString("yyyyMMdd-HHmmss")
$TimestampIso = $UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
$ShortGuid = [Guid]::NewGuid().ToString("N").Substring(0, 8).ToUpperInvariant()
$BuildId = "BLD-$TimestampCompact-$ShortGuid"

Write-Host "====================================================================" -ForegroundColor Cyan
Write-Host " Rouvr AudioTool Standardized Build Pipeline" -ForegroundColor Cyan
Write-Host " Manual Version : $ManualVersion" -ForegroundColor Green
Write-Host " Unique BuildID : $BuildId" -ForegroundColor Green
Write-Host "====================================================================" -ForegroundColor Cyan

# Ensure Git local user config exists so commits succeed cleanly
$GitUser = git config user.name 2>$null
if ([string]::IsNullOrWhiteSpace($GitUser)) {
    git config user.name "Rouvr Builder"
    git config user.email "builder@rouvr.local"
}

function Write-VersionHeader([string]$GitSha) {
    $HeaderPath = Join-Path $ProjectRoot "include\at_version.h"
    $HeaderContent = @"
#pragma once
// ============================================================================
// AUTO-GENERATED VERSION & BUILD ID HEADER (Updated by scripts/build.ps1)
// The semantic version is read from VERSION.txt (supplied manually).
// The unique build ID is generated on every pipeline execution.
// ============================================================================

#define AT_VERSION_STRING      "$ManualVersion"
#define AT_BUILD_ID            "$BuildId"
#define AT_BUILD_TIMESTAMP_UTC "$TimestampIso"
#define AT_GIT_COMMIT_HASH     "$GitSha"
"@
    Set-Content -Path $HeaderPath -Value $HeaderContent -Encoding UTF8
}

# 3. Write version header and commit changes to Git
$ParentSha = (git rev-parse --short HEAD 2>$null)
if ([string]::IsNullOrWhiteSpace($ParentSha)) {
    $ParentSha = "ROOT"
} else {
    $ParentSha = $ParentSha.Trim()
}
Write-VersionHeader -GitSha $ParentSha

if (-not $SkipCommit) {
    git add -A
    $Status = git status --porcelain
    if (-not [string]::IsNullOrWhiteSpace($Status)) {
        if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
            $CommitMessage = "Build v$ManualVersion [$BuildId]"
        }
        Write-Host "[Git] Committing workspace changes: $CommitMessage" -ForegroundColor Yellow
        git commit -m "$CommitMessage" | Out-Host
    }
}

# 4. Locate Visual Studio 2022 CMake, Ninja, and vcvars64.bat
$VsBase = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$VcVars64 = Join-Path $VsBase "VC\Auxiliary\Build\vcvars64.bat"
$CMakeExe = Join-Path $VsBase "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$CTestExe = Join-Path $VsBase "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$NinjaExe = Join-Path $VsBase "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if (-not (Test-Path $CMakeExe)) { $CMakeExe = "cmake.exe" }
if (-not (Test-Path $CTestExe)) { $CTestExe = "ctest.exe" }

$BuildDir = Join-Path $ProjectRoot "build"

# 5. Configure & Build via CMake + Ninja inside x64 MSVC Developer Environment
$CmdScript = @"
call "$VcVars64" >nul
"$CMakeExe" -S "$ProjectRoot" -B "$BuildDir" -G Ninja -DCMAKE_MAKE_PROGRAM="$NinjaExe" -DCMAKE_BUILD_TYPE=$BuildType
if errorlevel 1 exit /b 1
"$CMakeExe" --build "$BuildDir" --config $BuildType
if errorlevel 1 exit /b 1
"@

$TempBat = Join-Path $env:TEMP "at_cmake_build.bat"
Set-Content -Path $TempBat -Value $CmdScript -Encoding ASCII
& cmd.exe /c $TempBat
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

# 6. Optional Local Self-Signed Certificate Signing (if configured per docs/06)
$LocalCert = Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -like "*Rouvr AudioTool Local Build*" -and $_.HasPrivateKey } |
    Select-Object -First 1

if ($null -ne $LocalCert) {
    Write-Host "[Security] Signing built binaries with Local Code Signing Cert ($($LocalCert.Thumbprint))..." -ForegroundColor Green
    Get-ChildItem -Path (Join-Path $BuildDir "bin") -Include *.exe, *.dll -Recurse | ForEach-Object {
        Set-AuthenticodeSignature -FilePath $_.FullName -Certificate $LocalCert | Out-Null
    }
    Get-ChildItem -Path "$env:LOCALAPPDATA\Rouvr\AudioTool\plugins" -Filter *.dll -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
        Set-AuthenticodeSignature -FilePath $_.FullName -Certificate $LocalCert | Out-Null
    }
}

# 7. Run Automated CTest Suite
if (-not $SkipTests) {
    Write-Host "[CTest] Running automated Proof-of-Concept test suite..." -ForegroundColor Cyan
    & $CTestExe --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Automated tests failed!"
    }
}

Write-Host "====================================================================" -ForegroundColor Green
Write-Host " Build Succeeded! Version: $ManualVersion | Build ID: $BuildId" -ForegroundColor Green
Write-Host " Executable : $BuildDir\bin\at_engine_poc.exe" -ForegroundColor Green
Write-Host " Plugins    : $env:LOCALAPPDATA\Rouvr\AudioTool\plugins" -ForegroundColor Green
Write-Host "====================================================================" -ForegroundColor Green
