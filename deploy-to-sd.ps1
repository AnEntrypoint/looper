$ErrorActionPreference = 'Stop'
$Repo = 'AnEntrypoint/looper'
$Drive = 'E:'

if (-not (Test-Path "$Drive\")) {
    Write-Error "E: drive not found. Insert SD card and try again."
    exit 1
}

$vol = Get-Volume -DriveLetter E -ErrorAction SilentlyContinue
if (-not $vol -or $vol.FileSystemType -ne 'FAT32') {
    Write-Warning "E: is not FAT32. Format it as FAT32 before continuing."
    exit 1
}

Write-Host "Fetching latest release from $Repo..."
$release = Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/tags/latest"
$asset = $release.assets | Where-Object { $_.name -eq 'looper-sd.zip' }

if (-not $asset) {
    Write-Error "looper-sd.zip not found in latest release. Has the build workflow run?"
    exit 1
}

$tmp = Join-Path $env:TEMP 'looper-sd'
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
New-Item -ItemType Directory $tmp | Out-Null

$zip = "$tmp\looper-sd.zip"
Write-Host "Downloading $($asset.browser_download_url)..."
Invoke-WebRequest $asset.browser_download_url -OutFile $zip

Write-Host "Extracting to $Drive..."
Expand-Archive $zip -DestinationPath $tmp -Force

$files = Get-ChildItem "$tmp" -Exclude '*.zip'
foreach ($f in $files) {
    Copy-Item $f.FullName -Destination "$Drive\" -Recurse -Force
}

Write-Host "Done. SD card at $Drive is ready to boot."
Write-Host "Safely eject $Drive, insert into rPi, and power on."
