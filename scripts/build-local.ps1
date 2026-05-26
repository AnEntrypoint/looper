# build-local.ps1 — local Windows mirror of .github/workflows/build.yml
#
# Produces dist/looper-sd.zip (kernel7l.img + boot files) without GitHub Actions.
# First-run does full clone + library build (~10-30min). Incremental rebuilds
# touch only the Looper sources (~seconds).
#
# Requires:
#   scoop install gcc-arm-none-eabi    (installed via "scoop install gcc-arm-none-eabi")
#   python3, git, 7z or Compress-Archive
#
# Layout:
#   $env:LOOPER_BUILD_ROOT (default: C:\dev\looper-build) holds circle/circle-prh checkouts.
#   This repo lives at C:\dev\looper and is mounted into circle/_prh/_apps/Looper via junction.

[CmdletBinding()]
param(
    [string]$BuildRoot = "$env:USERPROFILE\.looper-build",
    [switch]$Clean,
    [switch]$LibsOnly,
    [switch]$AppOnly,
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Find-ArmGcc {
    $candidates = @(
        "$env:USERPROFILE\scoop\apps\gcc-arm-none-eabi\current\bin",
        "C:\Program Files (x86)\GNU Arm Embedded Toolchain\*\bin",
        "C:\Program Files\GNU Arm Embedded Toolchain\*\bin"
    )
    foreach ($p in $candidates) {
        $resolved = Get-Item -ErrorAction SilentlyContinue $p | Select-Object -First 1
        if ($resolved -and (Test-Path (Join-Path $resolved.FullName 'arm-none-eabi-gcc.exe'))) {
            return $resolved.FullName
        }
    }
    return $null
}

$armBin = Find-ArmGcc
if (-not $armBin) {
    Write-Error "arm-none-eabi-gcc not found. Run: scoop install gcc-arm-none-eabi"
    exit 1
}
$env:PATH = "$armBin;$env:PATH"
Write-Host "[build-local] using toolchain: $armBin"
& arm-none-eabi-gcc --version | Select-Object -First 1

# bash from git-for-windows is required to run patches/patch_rules_mk.py-style POSIX commands
$bash = (Get-Command bash -ErrorAction SilentlyContinue).Path
if (-not $bash) { Write-Error 'bash not found (install git-for-windows)'; exit 1 }
# git-bash starts with its own PATH from its profile and does NOT reliably
# inherit the Windows-style $armBin we prepended above, so `make` can't find
# arm-none-eabi-g++. Convert the toolchain dir to a bash path and prepend it
# inside every `make` invocation via $makeEnv.
$armBinBash = '/' + ($armBin -replace ':','' -replace '\\','/')   # C:\x\bin -> /C/x/bin
$makeEnv = "export PATH=`"$armBinBash`:`$PATH`"; "

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$circle = Join-Path $BuildRoot 'circle'
$prh    = Join-Path $circle '_prh'
$appDir = Join-Path $prh '_apps\Looper'

if ($Clean) {
    Write-Host '[build-local] CLEAN: removing $BuildRoot'
    Remove-Item -Recurse -Force $BuildRoot -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
}

# 1. Clone or update circle + circle-prh
if (-not (Test-Path (Join-Path $circle '.git'))) {
    Write-Host '[build-local] cloning rsta2/circle ...'
    & git clone --depth=1 https://github.com/rsta2/circle "$circle"
}
if (-not (Test-Path (Join-Path $prh '.git'))) {
    Write-Host '[build-local] cloning phorton1/circle-prh ...'
    & git clone --depth=1 https://github.com/phorton1/circle-prh "$prh"
}

# 2. Mirror this repo into circle/_prh/_apps/Looper (robocopy — fast incremental)
# We don't junction because the build process needs to copy patches/* up to the
# app dir root, which would pollute the live working tree under a junction.
$appsDir = Join-Path $prh '_apps'
New-Item -ItemType Directory -Force -Path $appsDir | Out-Null
if (Test-Path $appDir) {
    $item = Get-Item $appDir
    if ($item.LinkType -eq 'Junction') {
        Write-Host "[build-local] removing legacy junction $appDir"
        Remove-Item $appDir -Force
    }
}
New-Item -ItemType Directory -Force -Path $appDir | Out-Null
Write-Host "[build-local] mirroring $RepoRoot -> $appDir (robocopy)"
$rcArgs = @($RepoRoot, $appDir, '/MIR', '/NJH', '/NJS', '/NDL', '/NP', '/NC', '/NS', '/XD', '.git', '.gm', 'dist', 'node_modules', '.github', 'circle-prh', '.looper-build', '/XF', '*.zip')
& robocopy @rcArgs | Out-Null
# robocopy exit codes 0-7 are success
if ($LASTEXITCODE -ge 8) { throw "robocopy failed: $LASTEXITCODE" }

# 3. Apply patches (mirror of build.yml)
# Restore the circle-prh tree first: a prior interrupted run can leave tracked
# upstream files (e.g. audio/bcm_pcm.h) deleted, which breaks the case-symlink.
# `git checkout` brings them all back so patching starts from a clean upstream.
Write-Host '[build-local] restoring circle-prh working tree ...'
Push-Location $prh
& git checkout -- . 2>&1 | Out-Null
Pop-Location
Write-Host '[build-local] applying patches ...'
$patches = @(
    # Audio path
    @('audio/bcm_pcm.h',          'audio/BCM_PCM.h',           'symlink'),
    @('patches/miniuart.cpp',     'utils/miniuart.cpp',        'copy'),
    @('patches/AudioTypes.h',     'audio/AudioTypes.h',        'copy'),
    @('patches/audio_Makefile',   'audio/Makefile',            'copy'),
    @('patches/utils_Makefile',   'utils/Makefile',            'copy'),
    @('patches/input_usb.h',      'audio/input_usb.h',         'copy'),
    @('patches/input_usb.cpp',    'audio/input_usb.cpp',       'copy'),
    @('patches/output_usb.h',     'audio/output_usb.h',        'copy'),
    @('patches/output_usb.cpp',   'audio/output_usb.cpp',      'copy'),
    @('patches/usbaudiodevice.h', 'audio/usbaudiodevice.h',    'copy'),
    @('patches/usbaudiodevice.cpp','audio/usbaudiodevice.cpp', 'copy'),
    @('patches/AudioSystem.cpp',  'audio/AudioSystem.cpp',     'copy'),
    @('patches/std_kernel_stub.h','system/std_kernel.h',       'copy'),
    @('patches/audioTelemetry.h', 'audio/audioTelemetry.h',    'copy'),
    @('patches/audioTelemetry.cpp','audio/audioTelemetry.cpp', 'copy'),
    @('patches/input_otg.h',      'audio/input_otg.h',         'copy'),
    @('patches/input_otg.cpp',    'audio/input_otg.cpp',       'copy'),
    @('patches/output_otg.h',     'audio/output_otg.h',        'copy'),
    @('patches/output_otg.cpp',   'audio/output_otg.cpp',      'copy'),
    @('patches/usbaudiogadget.h', 'audio/usbaudiogadget.h',    'copy'),
    @('patches/usbaudiogadget.cpp','audio/usbaudiogadget.cpp', 'copy'),
    @('patches/usbaudiogadgetendpoint.h','audio/usbaudiogadgetendpoint.h','copy'),
    @('patches/usbaudiogadgetendpoint.cpp','audio/usbaudiogadgetendpoint.cpp','copy')
)
# Targets relative to circle/_prh
foreach ($p in $patches) {
    $src = Join-Path $RepoRoot $p[0]
    $dst = Join-Path $prh $p[1]
    $mode = $p[2]
    $dstDir = Split-Path $dst -Parent
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    if ($mode -eq 'symlink') {
        # CI does `ln -sf bcm_pcm.h circle/_prh/audio/BCM_PCM.h` to satisfy the
        # uppercase #include on Linux's case-SENSITIVE filesystem. Windows is
        # case-INSENSITIVE: "BCM_PCM.h" already resolves to bcm_pcm.h, so the
        # case-copy is unnecessary — and worse, Copy-Item/Remove-Item of the
        # uppercase name operates on the SAME inode as the lowercase file,
        # deleting the real source. So on Windows: ensure the lowercase source
        # exists (restore from the prh git tree if a prior run removed it) and
        # SKIP the case-copy entirely.
        $leaf = Split-Path $p[0] -Leaf
        $linkSrc = Join-Path $dstDir $leaf
        if (-not (Test-Path $linkSrc)) {
            Push-Location $prh
            & git checkout -- (Join-Path 'audio' $leaf) 2>&1 | Out-Null
            Pop-Location
        }
        # no copy: case-insensitive FS makes the uppercase include resolve already
    } else {
        Copy-Item -Path $src -Destination $dst -Force
    }
}

# circle-clone net-race fix: patched CNetBufferQueue::Dequeue (reads m_pFirst
# inside the spinlock) — targets circle/lib/net, not the _prh tree.
$netSrc = Join-Path $RepoRoot 'patches/circle_netbufferqueue.cpp'
$netDst = Join-Path $circle 'lib/net/netbufferqueue.cpp'
if (Test-Path $netSrc) { Copy-Item -Path $netSrc -Destination $netDst -Force }

# app-dir-internal patches (Makefile expects these next to itself, not in patches/)
$appInternal = @(
    'kernel.h','kernel.cpp','kernel_run.cpp','multicore.cpp',
    'coreDispatch.h','coreDispatch.cpp','coreBusy.h','coreBusy.cpp',
    'paramSnapshot.h','paramSnapshot.cpp','main.cpp'
)
foreach ($name in $appInternal) {
    Copy-Item -Path (Join-Path $RepoRoot "patches\$name") -Destination (Join-Path $appDir $name) -Force
}

# circle/lib/usb/ patches
$libUsbPatches = @(
    'usbmidihost.cpp', 'usbconfigparser.cpp', 'usbdevice.cpp',
    'usbaudiodevice.h', 'usbaudiodevice.cpp', 'usbdevicefactory.cpp', 'usb_Makefile'
)
foreach ($name in $libUsbPatches) {
    $src = Join-Path $RepoRoot ("patches\$name")
    $dstName = if ($name -eq 'usb_Makefile') { 'Makefile' } else { $name }
    $dst = Join-Path $circle ("lib\usb\$dstName")
    Copy-Item -Path $src -Destination $dst -Force
}

# circle/lib/usb/gadget/
$gadgetPatches = @('dwusbgadgetendpoint.h', 'dwusbgadgetendpoint.cpp', 'dwusbgadgetendpoint0.cpp', 'dwusbgadget.cpp')
foreach ($name in $gadgetPatches) {
    $src = Join-Path $RepoRoot ("patches\$name")
    Copy-Item -Path $src -Destination (Join-Path $circle "lib\usb\gadget\$name") -Force
}
Copy-Item (Join-Path $RepoRoot 'patches\dwusbgadgetendpoint.h') (Join-Path $circle 'include\circle\usb\gadget\dwusbgadgetendpoint.h') -Force

# WLAN patches
Copy-Item (Join-Path $RepoRoot 'patches\p9chan.h')   (Join-Path $circle 'addon\wlan\p9chan.h') -Force
Copy-Item (Join-Path $RepoRoot 'patches\p9chan.cpp') (Join-Path $circle 'addon\wlan\p9chan.cpp') -Force

# ARM_ALLOW_MULTI_CORE flip
$sysconfig = Join-Path $circle 'include\circle\sysconfig.h'
# Enable multi-core + enlarge stacks: Core 2 (control plane) runs net/TCP
# scheduler tasks that overflowed the 32KB task stack -> Core-2 crashes
# (netbufferqueue asserts, prefetch-abort) ~50-90s after boot.
$sc = (Get-Content $sysconfig -Raw) -replace '//#define ARM_ALLOW_MULTI_CORE', '#define ARM_ALLOW_MULTI_CORE'
$sc = $sc -replace '#define TASK_STACK_SIZE\s+0x8000', "#define TASK_STACK_SIZE`t`t0x20000"
$sc = $sc -replace '#define KERNEL_STACK_SIZE\s+0x20000', "#define KERNEL_STACK_SIZE`t0x40000"
$sc | Set-Content -Encoding utf8 -NoNewline $sysconfig

# Rules.mk patch (Python) — opens 'circle/Rules.mk' relative, so run from the
# build root (the dir that contains circle/), not the looper repo cwd.
Push-Location $BuildRoot
& python (Join-Path $RepoRoot 'patches\patch_rules_mk.py')
Pop-Location

# 4. Build circle libs (incremental: skip if .a exists)
if (-not $AppOnly) {
    $libTargets = @(
        @{ dir = 'lib';            args = '' },
        @{ dir = 'lib/fs';         args = '' },
        @{ dir = 'lib/input';      args = '' },
        @{ dir = 'lib/sched';      args = '' },
        @{ dir = 'lib/usb';        args = '' },
        @{ dir = 'lib/usb/gadget'; args = '' },
        @{ dir = 'lib/net';        args = '' },
        @{ dir = 'addon/fatfs';    args = '' },
        @{ dir = 'addon/SDCard';   args = '' },
        @{ dir = 'addon/wlan';     args = '' }
    )
    foreach ($t in $libTargets) {
        Push-Location (Join-Path $circle $t.dir)
        try {
            $cmd = "$makeEnv make RASPPI=4 AARCH=32 ARM_ALLOW_MULTI_CORE=1 -j$Jobs"
            Write-Host "[build-local] $($t.dir): $cmd"
            & $bash -c $cmd
            if ($LASTEXITCODE -ne 0) { throw "make failed in $($t.dir)" }
        } finally { Pop-Location }
    }
    # circle-prh libs
    Push-Location (Join-Path $prh 'utils')
    & $bash -c "$makeEnv make RASPPI=4 AARCH=32 ARM_ALLOW_MULTI_CORE=1 -j$Jobs"
    Pop-Location
    Push-Location (Join-Path $prh 'audio')
    & $bash -c "$makeEnv make RASPPI=4 AARCH=32 LOOPER_USB_AUDIO=1 LOOPER_OTG_AUDIO=1 ARM_ALLOW_MULTI_CORE=1 -j$Jobs"
    Pop-Location
}

if ($LibsOnly) { Write-Host '[build-local] LibsOnly done.'; exit 0 }

# 5. WiFi firmware (cached after first run)
$fwDir = Join-Path $appDir 'dist\firmware'
New-Item -ItemType Directory -Force -Path $fwDir | Out-Null
$fwSha = 'c9d3ae6584ab79d19a4f94ccf701e888f9f87a53'
$fwBase = "https://github.com/RPi-Distro/firmware-nonfree/raw/$fwSha/debian/config/brcm80211"
$fwFiles = @(
    @('cypress/cyfmac43455-sdio-minimal.bin', 'brcmfmac43455-sdio.bin'),
    @('brcm/brcmfmac43455-sdio.txt',          'brcmfmac43455-sdio.txt'),
    @('cypress/cyfmac43455-sdio.clm_blob',    'brcmfmac43455-sdio.clm_blob')
)
foreach ($f in $fwFiles) {
    $dst = Join-Path $fwDir $f[1]
    if (-not (Test-Path $dst)) {
        Write-Host "[build-local] fetching $($f[1])"
        Invoke-WebRequest -Uri "$fwBase/$($f[0])" -OutFile $dst -UseBasicParsing
    }
}

# 6. Generate embedded firmware
$wlanS = Join-Path $env:TEMP 'wlan_firmware.S'
& python (Join-Path $RepoRoot 'patches\gen_wlan_firmware.py') $fwDir $wlanS
# gen_wlan_firmware.py emits .incbin paths with Windows backslashes, which the
# GNU assembler reads as escapes -> "file not found". Rewrite to forward slashes.
(Get-Content $wlanS -Raw) -replace '\\','/' | Set-Content $wlanS -Encoding ascii
& arm-none-eabi-as -mcpu=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard `
    -o (Join-Path $appDir 'wlan_firmware.o') $wlanS

# 7. Build looper app
Copy-Item -Recurse -Force (Join-Path $appDir 'patches\rubberband')  $appDir
Copy-Item -Recurse -Force (Join-Path $appDir 'patches\signalsmith') $appDir
Push-Location $appDir
try {
    & $bash -c "$makeEnv make RASPPI=4 AARCH=32 LOOPER_USB_AUDIO=1 LOOPER_OTG_AUDIO=1 ARM_ALLOW_MULTI_CORE=1 CHECK_DEPS=0 -j$Jobs"
    if ($LASTEXITCODE -ne 0) { throw 'Looper build failed' }
} finally { Pop-Location }

# 8. Assemble dist/looper-sd.zip
$dist = Join-Path $RepoRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item (Join-Path $appDir 'kernel7l.img') $dist -Force
Copy-Item (Join-Path $circle 'boot\*') $dist -Recurse -Force
Copy-Item $fwDir (Join-Path $dist 'firmware') -Recurse -Force
'usbspeed=full' | Out-File -Encoding ascii -NoNewline (Join-Path $dist 'cmdline.txt')
@"
[all]
boot_delay=0
disable_splash=1
gpu_mem=64

[pi4]
arm_64bit=0
kernel=kernel7l.img
"@ | Out-File -Encoding ascii (Join-Path $dist 'config.txt')

$zip = Join-Path $RepoRoot 'dist\looper-sd.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $dist '*') -DestinationPath $zip -CompressionLevel Optimal -Force
Write-Host "[build-local] DONE: $zip"
Write-Host "Deploy: .\netboot-deploy.ps1   (will push kernel7l.img to TFTP root)"
