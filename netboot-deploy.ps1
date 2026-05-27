param(
    # Must match tftp-server.js TFTPROOT (it serves <repo>\tftproot, NOT C:\tftproot).
    # Deploying to the wrong root silently leaves the Pi booting a stale kernel.
    [string]$TftpRoot = (Join-Path $PSScriptRoot "tftproot"),
    [string]$RpiSerial = ""
)

$zip = Join-Path $PSScriptRoot "dist\looper-sd.zip"

if (-not (Test-Path $zip)) {
    Write-Error "looper-sd.zip not found. Run: gh release download latest --repo AnEntrypoint/looper --pattern looper-sd.zip --dir dist"
    exit 1
}

$tmp = Join-Path $env:TEMP "looper-netboot-$$"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
Expand-Archive -Path $zip -DestinationPath $tmp -Force

$dest = if ($RpiSerial) { Join-Path $TftpRoot $RpiSerial } else { $TftpRoot }
New-Item -ItemType Directory -Force -Path $dest | Out-Null

Copy-Item "$tmp\kernel7l.img" $dest -Force
Copy-Item "$tmp\*.dat" $dest -Force -ErrorAction SilentlyContinue
Copy-Item "$tmp\*.elf" $dest -Force -ErrorAction SilentlyContinue
Copy-Item "$tmp\*.bin" $dest -Force -ErrorAction SilentlyContinue
Copy-Item "$tmp\cmdline.txt" $dest -Force -ErrorAction SilentlyContinue
Copy-Item "$tmp\config.txt" $dest -Force -ErrorAction SilentlyContinue

# rPi4 netboots from tftproot\<cpu-serial>\ (e.g. 7bec0617), NOT the root.
# Mirror the kernel into every existing serial subdir so the live boot path is
# always updated — copying to the root alone leaves the Pi on a stale kernel.
if (-not $RpiSerial) {
    Get-ChildItem -Path $TftpRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'kernel7l.img') } |
        ForEach-Object {
            Copy-Item "$tmp\kernel7l.img" $_.FullName -Force
            Write-Host "Deployed kernel7l.img to $($_.FullName)"
        }
}

Remove-Item $tmp -Recurse -Force
Write-Host "Deployed kernel7l.img to $dest"
Write-Host ""
Write-Host "To enable rPi4 netboot (one-time, on the rPi4):"
Write-Host "  sudo raspi-config nonint do_boot_order B3   # Network boot first"
Write-Host "  rpi-eeprom-update -a && reboot"
Write-Host ""
Write-Host "Or set EEPROM boot order via SD once:"
Write-Host "  Add 'BOOT_ORDER=0x21' to /boot/bootconf.txt then flash with rpi-eeprom-update"
