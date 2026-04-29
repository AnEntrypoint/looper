param(
    [string]$TftpRoot = "C:\tftproot",
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

Remove-Item $tmp -Recurse -Force
Write-Host "Deployed kernel7l.img to $dest"
Write-Host ""
Write-Host "To enable rPi4 netboot (one-time, on the rPi4):"
Write-Host "  sudo raspi-config nonint do_boot_order B3   # Network boot first"
Write-Host "  rpi-eeprom-update -a && reboot"
Write-Host ""
Write-Host "Or set EEPROM boot order via SD once:"
Write-Host "  Add 'BOOT_ORDER=0x21' to /boot/bootconf.txt then flash with rpi-eeprom-update"
