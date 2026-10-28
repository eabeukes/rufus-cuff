# Talk to the Linux shell on the watch's USB serial gadget (g_serial -> "USB Serial Device (COMx)").
#   watch-shell.ps1 [-WaitSeconds 90] [-Commands 'touch /tmp/stay','uname -a']
# Waits for the port to appear, runs each command, prints what comes back.
param(
    [int]$WaitSeconds = 90,
    [string[]]$Commands = @('touch /tmp/stay', 'uname -a')
)

$port = $null
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while (-not $port -and (Get-Date) -lt $deadline) {
    $dev = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match 'USB Serial Device \((COM\d+)\)' } | Select-Object -First 1
    if ($dev) { $port = $Matches[1] } else { Start-Sleep -Milliseconds 500 }
}
if (-not $port) { Write-Output "no USB Serial Device appeared within $WaitSeconds s"; exit 1 }
Write-Output "found $port ($($dev.InstanceId))"

$sp = New-Object System.IO.Ports.SerialPort $port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.NewLine = "`n"
$sp.DtrEnable = $true
$sp.Open()
Start-Sleep -Milliseconds 800
$sp.Write("`n")
Start-Sleep -Milliseconds 500
$null = $sp.ReadExisting()

foreach ($c in $Commands) {
    $sp.Write("$c`n")
    $out = ''
    $quiet = 0
    while ($quiet -lt 4) {            # stop after ~2 s without new output
        Start-Sleep -Milliseconds 500
        $chunk = $sp.ReadExisting()
        if ($chunk) { $out += $chunk; $quiet = 0 } else { $quiet++ }
    }
    Write-Output "### $c"
    Write-Output ($out -replace "`r", '')
}
$sp.Close()
