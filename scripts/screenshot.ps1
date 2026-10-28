# Screenshot of the Linux framebuffer over the USB serial shell (COM port of g_serial).
#   screenshot.ps1 [-Name shot] [-Port COM4]   -> cam/<Name>.png in the wearer's 640x480 view
#   -Reuse sends an existing /tmp/fb.gz (captured on the watch at the right moment) instead.
#   -Grim captures the desktop (sway owns the display then, /dev/fb0 is stale) with grim.
# The watch snapshots /dev/fb0 to /tmp/fb.gz and prints it as base64; WSL's Python+PIL turns it
# into a PNG. The serial link occasionally drops bytes, so the transfer is retried until the gzip
# stream (which carries a CRC) decompresses to a whole frame.
param([string]$Name = 'shot', [string]$Port = 'COM4', [switch]$Reuse, [switch]$Grim)
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$gz = Join-Path $repo "cam\$Name.fb.gz"
New-Item -ItemType Directory -Force (Join-Path $repo 'cam') | Out-Null

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.DtrEnable = $true
$sp.ReadBufferSize = 8MB
$sp.Open()

function Run([string]$cmd, [int]$seconds) {
    $null = $sp.ReadExisting()
    # Markers are split with '' so the echoed command line does not contain them.
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

function FrameOk([byte[]]$bytes) {
    try {
        $z = New-Object IO.Compression.GZipStream((New-Object IO.MemoryStream(, $bytes)), [IO.Compression.CompressionMode]::Decompress)
        $out = New-Object IO.MemoryStream
        $z.CopyTo($out)
        return $out.Length -eq 480 * 640 * 4
    } catch { return $false }
}

Start-Sleep -Milliseconds 300
$src = '/tmp/fb.gz'
if ($Grim) {
    $src = '/tmp/grim.png'
    $null = Run "su rufus -c 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 grim $src'" 20
} elseif (-not $Reuse) {
    $null = Run 'dd if=/dev/fb0 bs=1920 count=640 2>/dev/null | gzip -c > /tmp/fb.gz' 20
}
# In 16 KiB chunks, each checked against its md5 on the watch and retried alone: a whole
# photo-like frame is too big to get through the serial link in one piece.
$md5 = [Security.Cryptography.MD5]::Create()
# Only a line that is just the number: job notices ("[1]+ Done ...") can land in the reply.
$size = [int][regex]::Match((Run "wc -c < $src" 10), '(?m)^\s*(\d+)\s*$').Groups[1].Value
$chunks = [Math]::Ceiling($size / 16384)
$all = New-Object IO.MemoryStream
for ($c = 0; $c -lt $chunks; $c++) {
    $got = $null
    for ($try = 1; $try -le 8 -and -not $got; $try++) {
        $r = Run "dd if=$src bs=16384 skip=$c count=1 2>/dev/null | md5sum | cut -c1-32; dd if=$src bs=16384 skip=$c count=1 2>/dev/null | base64" 30
        if (-not $r) { continue }
        $lines = ($r.Trim() -split "`n")
        $want = $lines[0].Trim()
        $b64 = ($lines | Select-Object -Skip 1) -join '' -replace '[^A-Za-z0-9+/=]', ''
        try { $cand = [Convert]::FromBase64String($b64) } catch { continue }
        $have = -join ($md5.ComputeHash($cand) | ForEach-Object { $_.ToString('x2') })
        if ($have -eq $want) { $got = $cand } else { Write-Output "chunk $c try $try corrupted" }
    }
    if (-not $got) { break }
    $all.Write($got, 0, $got.Length)
}
$bytes = $all.ToArray()
$sp.Close()
if ($Grim) {
    if ($bytes.Length -ne $size -or $size -eq 0) { Write-Output 'no intact screenshot received'; exit 1 }
    [IO.File]::WriteAllBytes((Join-Path $repo "cam\$Name.png"), $bytes)
    Join-Path $repo "cam\$Name.png"
    exit 0
}
if (-not (FrameOk $bytes)) { $bytes = $null }
if (-not $bytes) { Write-Output 'no intact framebuffer dump received'; exit 1 }
[IO.File]::WriteAllBytes($gz, $bytes)

$wslRepo = '/mnt/' + $repo.Substring(0, 1).ToLower() + ($repo.Substring(2) -replace '\\', '/')
wsl -d Ubuntu-24.04 -u builder -- python3 -I "$wslRepo/scripts/fb2png.py" "$wslRepo/cam/$Name.fb.gz" "$wslRepo/cam/$Name.png"
Remove-Item $gz
Join-Path $repo "cam\$Name.png"
