# Copy a file from the watch's Linux to the PC over the USB serial shell (g_serial).
#   watch-pull.ps1 -Remote /root/file -Local cam\file [-Port COM4]
# Sent in 16 KiB chunks, each checked against its md5 on the watch and retried alone
# (the serial link occasionally drops bytes), as screenshot.ps1 does.
param([Parameter(Mandatory)][string]$Remote, [Parameter(Mandatory)][string]$Local, [string]$Port = 'COM4')
$ErrorActionPreference = 'Stop'

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.DtrEnable = $true
$sp.ReadBufferSize = 8MB
$sp.Open()

function Run([string]$cmd, [int]$seconds) {
    $null = $sp.ReadExisting()
    $sp.Write("echo FB''START; $cmd; echo FB''STOP`n")
    $buf = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline -and -not $buf.ToString().Contains('FBSTOP')) {
        $c = $sp.ReadExisting()
        if ($c) { [void]$buf.Append($c) } else { Start-Sleep -Milliseconds 50 }
    }
    $s = $buf.ToString()
    $i = $s.IndexOf('FBSTART'); $j = $s.IndexOf('FBSTOP')
    if ($i -lt 0 -or $j -lt $i) { return $null }
    $s.Substring($i + 7, $j - $i - 7)
}

$md5 = [Security.Cryptography.MD5]::Create()
$size = [int][regex]::Match((Run "wc -c < $Remote" 10), '(?m)^\s*(\d+)\s*$').Groups[1].Value
$all = New-Object IO.MemoryStream
for ($c = 0; $c -lt [Math]::Ceiling($size / 16384); $c++) {
    $got = $null
    for ($try = 1; $try -le 8 -and -not $got; $try++) {
        $r = Run "dd if=$Remote bs=16384 skip=$c count=1 2>/dev/null | md5sum | cut -c1-32; dd if=$Remote bs=16384 skip=$c count=1 2>/dev/null | base64" 30
        if (-not $r) { continue }
        $lines = ($r.Trim() -split "`n")
        $b64 = ($lines | Select-Object -Skip 1) -join '' -replace '[^A-Za-z0-9+/=]', ''
        try { $cand = [Convert]::FromBase64String($b64) } catch { continue }
        $have = -join ($md5.ComputeHash($cand) | ForEach-Object { $_.ToString('x2') })
        if ($have -eq $lines[0].Trim()) { $got = $cand }
    }
    if (-not $got) { $sp.Close(); Write-Output "chunk $c failed"; exit 1 }
    $all.Write($got, 0, $got.Length)
}
$sp.Close()
if ($all.Length -ne $size) { Write-Output "got $($all.Length) of $size bytes"; exit 1 }
[IO.File]::WriteAllBytes($Local, $all.ToArray())
"$Local ($size bytes)"
