# Prints the serial output of the ESP32-C3. Opening the port can restart the board.
param(
  [string]$Port = 'COM5',
  [int]$Seconds = 30
)

$serial = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$serial.DtrEnable = $true
$serial.RtsEnable = $false
$serial.Open()
try {
  $end = (Get-Date).AddSeconds($Seconds)
  while ((Get-Date) -lt $end) {
    $text = $serial.ReadExisting()
    if ($text) { Write-Host -NoNewline $text }
    Start-Sleep -Milliseconds 100
  }
}
finally {
  $serial.Close()
}
