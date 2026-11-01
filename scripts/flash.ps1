# Run on the Windows machine the ESP32-C3 is plugged into.
# Fetches the binaries from the build host and flashes them over USB. This replaces the
# partition table, and the slot layout means stored images may need uploading again.
param(
  [string]$Port = 'COM5',
  [string]$BuildHost = 'wsl-dev',
  [string]$RemoteDir = 'code/personal/esp32-dm-pixel-display-poc/dist',
  [Parameter(Mandatory = $true)][string]$Esptool
)

$ErrorActionPreference = 'Stop'

$dist = Join-Path $env:TEMP 'esp32-dm-pixel-display-dist'
New-Item -ItemType Directory -Force $dist | Out-Null
scp -q "${BuildHost}:$RemoteDir/*.bin" $dist
if ($LASTEXITCODE -ne 0) { throw 'Could not fetch the binaries' }

& $Esptool --chip esp32c3 --port $Port --baud 460800 write-flash `
  0x0 "$dist\bootloader.bin" `
  0x8000 "$dist\partitions.bin" `
  0xe000 "$dist\boot_app0.bin" `
  0x10000 "$dist\firmware.bin"
if ($LASTEXITCODE -ne 0) { throw 'Flashing failed' }
