param([string]$Drive = "E")

$zip = Join-Path $PSScriptRoot "dist\looper-sd.zip"
$dest = "${Drive}:\"

if (-not (Test-Path $zip)) {
    Write-Error "looper-sd.zip not found at $zip. Run: gh release download latest --repo AnEntrypoint/looper --pattern looper-sd.zip --dir dist"
    exit 1
}

if (-not (Test-Path $dest)) {
    Write-Error "Drive $Drive`: not found. Insert SD card and retry."
    exit 1
}

Write-Host "Extracting $zip to $dest ..."
Expand-Archive -Path $zip -DestinationPath $dest -Force
Write-Host "Done. Eject $Drive`: and insert SD card into rPi4."
