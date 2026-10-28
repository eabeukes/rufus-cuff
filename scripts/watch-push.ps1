# Copy a file from the PC to the watch's Linux over the USB serial shell (g_serial).
#   watch-push.ps1 -Local images\file -Remote /usr/local/bin/file [-Mode 755] [-Port COM4]
# The reverse of watch-pull.ps1: gzipped, sent in 600-byte base64 lines (busybox ash's line
# editor takes about 1 KiB), each checked against its md5 on the watch and resent alone. The file replaces
# $Remote only once the whole of it checks out.
param([Parameter(Mandatory)][string]$Local, [Parameter(Mandatory)][string]$Remote,
    [string]$Mode = '', [string]$Port = 'COM4')
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
        if ($c) { [void]$buf.Append($c) } else { Start-Sleep -Milliseconds 20 }
    }
    $s = $buf.ToString()
    $i = $s.IndexOf('FBSTART'); $j = $s.IndexOf('FBSTOP')
    if ($i -lt 0 -or $j -lt $i) { return $null }
    $s.Substring($i + 7, $j - $i - 7).Trim()
}

function Md5([byte[]]$b) { -join ([Security.Cryptography.MD5]::Create().ComputeHash($b) | ForEach-Object { $_.ToString('x2') }) }

$raw = [IO.File]::ReadAllBytes((Resolve-Path $Local))
$ms = New-Object IO.MemoryStream
$gz = New-Object IO.Compression.GZipStream $ms, ([IO.Compression.CompressionLevel]::Optimal)
$gz.Write($raw, 0, $raw.Length); $gz.Close()
$data = $ms.ToArray()

$part = "$Remote.part"
$null = Run "rm -f $part.gz /tmp/push.chunk" 10
for ($off = 0; $off -lt $data.Length; $off += 600) {
    $n = [Math]::Min(600, $data.Length - $off)
    $chunk = New-Object byte[] $n
    [Array]::Copy($data, $off, $chunk, 0, $n)
    $want = Md5 $chunk
    $ok = $false
    for ($try = 1; $try -le 8 -and -not $ok; $try++) {
        $r = Run "echo $([Convert]::ToBase64String($chunk)) | base64 -d > /tmp/push.chunk && [ `$(md5sum < /tmp/push.chunk | cut -c1-32) = $want ] && cat /tmp/push.chunk >> $part.gz && echo CHUNK''OK" 10
        $ok = $r -and $r.EndsWith('CHUNKOK')
    }
    if (-not $ok) { $sp.Close(); Write-Output "chunk at $off failed"; exit 1 }
}
$r = Run "rm -f /tmp/push.chunk; gunzip -f $part.gz && md5sum < $part | cut -c1-32" 30
if (-not ($r -and $r.EndsWith((Md5 $raw)))) { $sp.Close(); Write-Output "md5 mismatch after unpacking: $r"; exit 1 }
if ($Mode) { $null = Run "chmod $Mode $part" 10 }
$null = Run "mv -f $part $Remote" 10
$sp.Close()
"$Remote ($($raw.Length) bytes, md5 verified)"
