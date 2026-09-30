# Guo Feeder - Cloud / Android local dev environment setup
#
# Purpose: redirect Node global packages, Android SDK/JDK/Gradle cache and AVD
#          to drive D: so that C: does not get bloated by npm / Gradle / SDK.
#
# NOTE: this script is written in ASCII only on purpose.
#       Windows PowerShell 5.1 reads .ps1 as ANSI when there is no BOM, so any
#       non-ASCII comment would corrupt parsing. Keep it ASCII.
#
# DRY-RUN by default (prints what it would do, changes nothing).
# To actually apply:
#   powershell -ExecutionPolicy Bypass -File setup-cloud-app-env.ps1 -Apply
#
# See docs/Cloud-APP-Platform-Plan.md section 5.

param(
    [switch]$Apply,
    [switch]$SkipAndroid
)

$ErrorActionPreference = "Stop"

# ---------- Path plan (all on D:) ----------
$ROOT           = "D:\Guo_Feeder_Project\tools"
$NODE_GLOBAL    = "$ROOT\node-global"
$NPM_CACHE      = "$ROOT\npm-cache"

$ANDROID_ROOT   = "D:\Android"
$ANDROID_SDK    = "$ANDROID_ROOT\Sdk"
$ANDROID_STUDIO = "$ANDROID_ROOT\Android Studio"
$JDK17          = "$ANDROID_ROOT\jdk17"
$GRADLE_HOME    = "$ANDROID_ROOT\.gradle"
$AVD_HOME       = "$ANDROID_ROOT\avd"

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Will($msg) { Write-Host "    [DRY-RUN] $msg" -ForegroundColor Yellow }
function DoIt($msg) { Write-Host "    [APPLY]   $msg" -ForegroundColor Green }

function Run([scriptblock]$block, $desc) {
    if ($Apply) { DoIt $desc; & $block } else { Will $desc }
}

Write-Host "================================================" -ForegroundColor Magenta
Write-Host " Guo Feeder - Cloud/Android dev env setup" -ForegroundColor Magenta
if ($Apply) {
    Write-Host " Mode: APPLY (WILL MODIFY YOUR SYSTEM)" -ForegroundColor Red
} else {
    Write-Host " Mode: DRY-RUN (safe, changes nothing)" -ForegroundColor Magenta
}
Write-Host "================================================" -ForegroundColor Magenta

# ---------- 0. Preflight ----------
Step "0. Preflight check"

$nodeCmd = Get-Command node -ErrorAction SilentlyContinue
if ($nodeCmd) {
    Write-Host "    node : $($nodeCmd.Source)"
    Write-Host "    node ver : $(node -v)"
} else {
    Write-Host "    node : NOT FOUND (install Node.js 18+ first)" -ForegroundColor Red
}

$npmCmd = Get-Command npm -ErrorAction SilentlyContinue
if ($npmCmd) {
    Write-Host "    npm  : $($npmCmd.Source)"
    Write-Host "    npm ver  : $(npm -v)"
}

$dDrive = Get-PSDrive D -ErrorAction SilentlyContinue
if ($dDrive) {
    $freeGB = [math]::Round($dDrive.Free / 1GB, 1)
    Write-Host "    D: free  : $freeGB GB"
    if ((-not $SkipAndroid) -and ($freeGB -lt 35)) {
        Write-Host "    WARNING: Android Studio + SDK + AVD needs about 25-30 GB." -ForegroundColor Yellow
        Write-Host "             Re-run with -SkipAndroid to skip the Android part." -ForegroundColor Yellow
    }
}

# ---------- 1. Directories ----------
Step "1. Create directories on D:"

$dirs = @($ROOT, $NODE_GLOBAL, $NPM_CACHE)
if (-not $SkipAndroid) {
    $dirs += @($ANDROID_ROOT, $ANDROID_SDK, $GRADLE_HOME, $AVD_HOME)
}
foreach ($d in $dirs) {
    Run { New-Item -ItemType Directory -Force -Path $d | Out-Null } "mkdir $d"
}

# ---------- 2. npm prefix / cache -> D: ----------
Step "2. Point npm global prefix and cache to D:"

Run { npm config set prefix "$NODE_GLOBAL" } "npm config set prefix $NODE_GLOBAL"
Run { npm config set cache  "$NPM_CACHE"   } "npm config set cache  $NPM_CACHE"

# ---------- 3. Frontend / server tooling ----------
Step "3. Install Wrangler (Cloudflare CLI) and pnpm"

Run { npm install -g wrangler } "npm install -g wrangler"
Run { npm install -g pnpm }     "npm install -g pnpm"

$corepackCmd = Get-Command corepack -ErrorAction SilentlyContinue
if ($corepackCmd) {
    Run { corepack enable pnpm } "corepack enable pnpm (fallback path)"
}

# ---------- 4. Android env vars ----------
Step "4. Set user environment variables (Android)"

if ($SkipAndroid) {
    Write-Host "    skipped (-SkipAndroid)" -ForegroundColor DarkGray
} else {
    $envMap = @{
        "JAVA_HOME"        = $JDK17
        "ANDROID_HOME"     = $ANDROID_SDK
        "ANDROID_SDK_ROOT" = $ANDROID_SDK
        "ANDROID_AVD_HOME" = $AVD_HOME
        "GRADLE_USER_HOME" = $GRADLE_HOME
    }
    foreach ($k in $envMap.Keys) {
        $v = $envMap[$k]
        Run { [Environment]::SetEnvironmentVariable($k, $v, "User") } "set $k = $v"
    }

    $pathAdd = @(
        "$ANDROID_SDK\platform-tools",
        "$ANDROID_SDK\cmdline-tools\latest\bin"
    )
    $curPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (-not $curPath) { $curPath = "" }
    $needAdd = @($pathAdd | Where-Object { $curPath -notlike "*$_*" })

    if ($needAdd.Count -gt 0) {
        $joined  = ($needAdd -join ";")
        $newPath = ($curPath.TrimEnd(";") + ";" + $joined).TrimStart(";")
        Run { [Environment]::SetEnvironmentVariable("Path", $newPath, "User") } "append to user PATH: $joined"
    } else {
        Write-Host "    PATH already contains Android tools" -ForegroundColor DarkGray
    }
}

# ---------- 5. Manual steps ----------
Step "5. Manual steps (cannot be scripted)"
Write-Host "    a) Android Studio - install to: $ANDROID_STUDIO"
Write-Host "       https://developer.android.com/studio"
Write-Host "       Choose Custom setup and set SDK path to: $ANDROID_SDK"
Write-Host "    b) JDK 17 (Temurin) - install to: $JDK17"
Write-Host "       https://adoptium.net/temurin/releases/?version=17"
Write-Host "    c) Cloudflare login:  wrangler login  ;  wrangler whoami"
Write-Host "    d) Create D1 database:  wrangler d1 create guo_feeder"
Write-Host "       then paste the returned database_id into wrangler.toml"
Write-Host "    e) Android SDK components (after Android Studio install):"
Write-Host "       sdkmanager --list"
Write-Host '       sdkmanager "platform-tools" "platforms;android-34" "build-tools;34.0.0"'
Write-Host "    f) Restart terminal / VS Code so the new env vars take effect."

# ---------- 6. Verification list ----------
Step "6. Verify in a NEW terminal"
$verify = @(
    "node -v",
    "npm -v",
    "npm config get prefix",
    "npm config get cache",
    "wrangler --version",
    "pnpm -v",
    "java -version",
    "adb version"
)
foreach ($c in $verify) { Write-Host "    $c" -ForegroundColor Gray }

Write-Host ""
Write-Host "================================================" -ForegroundColor Magenta
if ($Apply) {
    Write-Host " APPLIED. Open a NEW terminal and run the checks above." -ForegroundColor Green
} else {
    Write-Host " DRY-RUN finished. Nothing was changed." -ForegroundColor Yellow
    Write-Host " To apply:" -ForegroundColor Yellow
    Write-Host "   powershell -ExecutionPolicy Bypass -File setup-cloud-app-env.ps1 -Apply"
    Write-Host " Add -SkipAndroid to skip the Android part."
}
Write-Host "================================================" -ForegroundColor Magenta
