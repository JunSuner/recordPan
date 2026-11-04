# Copies recordings from the board's flash storage partition to recordings/.
# Usage: .\tools\pull_recordings.ps1 -Port COM7
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [string]$IdfPath = "C:\Espressif\v6.1\esp-idf"
)
# Must match the "storage" entry in firmware/partitions.csv.
$StorageOffset = "0x310000"
$StorageSize = "0xC00000"

$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path ([System.IO.Path]::GetTempPath()) "recordpan_pull"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$work\extract" | Out-Null

. "$IdfPath\export.ps1" *> $null

# watchdog-reset brings the app back up afterwards; a plain RTS reset can leave
# the native USB port in download mode.
esptool --chip esp32s3 -p $Port -b 921600 --before default-reset --after watchdog-reset `
    read-flash $StorageOffset $StorageSize "$work\storage.bin" | Select-String "Read |rror"
if ($LASTEXITCODE -ne 0) { throw "read-flash failed" }

Push-Location "$work\extract"
python "$IdfPath\components\fatfs\fatfsparse.py" --wl-layer enabled "$work\storage.bin"
$parseExit = $LASTEXITCODE
Pop-Location
if ($parseExit -ne 0) { throw "fatfsparse failed" }

$dest = Join-Path $root "recordings"
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem -Recurse -File "$work\extract" | ForEach-Object {
    # Keep the original case: REC_NNNN.json refers to its audio file by name.
    Copy-Item $_.FullName (Join-Path $dest $_.Name) -Force
    "{0,-20} {1,10:N0} bytes" -f $_.Name, $_.Length
}
