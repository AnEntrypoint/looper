# Engine-sub-stage sweep — isolate which CC103-107 stage produces the periodic
# glitch the user reports when transpose is engaged. Each iteration toggles one
# stage off (others at default), records 3 s through OTG↔OTG, counts cuts.
# The config with FEWEST cuts → the offending stage.
#
# Use:
#   .\scripts\engine-sweep.ps1
param(
    [string]$PiIP = '192.168.137.100',
    [int]$Sec = 3,
    [int]$Freq = 200
)
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($PiIP), 4446)
function Send-CC([byte]$cc, [byte]$val) {
    $c = New-Object System.Net.Sockets.UdpClient
    $msg = [byte[]](0xB0, $cc, $val)
    $c.Send($msg, $msg.Length, $ep) | Out-Null
    $c.Close()
}
function Engage-Unity {
    $c = New-Object System.Net.Sockets.UdpClient
    $msg = [byte[]](0x92, 60, 127)  # ch2 note 60 = engage+semis=0
    $c.Send($msg, $msg.Length, $ep) | Out-Null
    $c.Close()
}
function Disengage {
    $c = New-Object System.Net.Sockets.UdpClient
    $msg = [byte[]](0xB0, 1, 64)
    $c.Send($msg, $msg.Length, $ep) | Out-Null
    $c.Close()
}

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$root = "C:\dev\looper\scripts\measure-results\engine-sweep-$stamp"
New-Item -ItemType Directory -Force -Path $root | Out-Null
$results = @()

function Run-Trial([string]$label, [scriptblock]$setup) {
    Disengage
    Start-Sleep -Milliseconds 200
    # reset all toggles to default
    Send-CC 103 0   # pre-bypass off (engine runs normally; auto-bypass when depth=0)
    Send-CC 104 127 # spliceSnap on
    Send-CC 105 127 # spliceMatch on
    Send-CC 106 32  # driftLow ~8
    Send-CC 107 32  # driftHigh ~256 → (16+32/127*1008)=270, close enough
    Start-Sleep -Milliseconds 200
    & $setup
    Start-Sleep -Milliseconds 200
    Engage-Unity
    Start-Sleep -Milliseconds 300
    $od = Join-Path $root "trial-${label}"
    $cap = & 'C:\dev\looper\scripts\loopback-cli.exe' --out Looper --in Looper --freq $Freq --sec $Sec --outdir $od
    # Mono + count cuts
    $wav = Join-Path $od 'cap.wav'
    $mono = Join-Path $od 'mono.wav'
    & ffmpeg -y -i $wav -ac 1 $mono 2>&1 | Out-Null
    $cuts = & 'C:\dev\looper\scripts\find-cuts.exe' $mono | Select-String "total cuts:"
    $cutNum = ($cuts -replace '.*total cuts:\s*(\d+).*', '$1').Trim()
    $line = "[$label] $cap | cuts=$cutNum"
    Write-Host $line
    $results += $line
    Disengage
    Start-Sleep -Milliseconds 200
}

Write-Host "=== ENGINE SUB-STAGE SWEEP ==="
Run-Trial 'baseline-all-on' { }
Run-Trial 'pre-bypass-on' { Send-CC 103 127 }
Run-Trial 'splice-snap-off' { Send-CC 104 0 }
Run-Trial 'splice-match-off' { Send-CC 105 0 }
Run-Trial 'snap+match-off' { Send-CC 104 0; Send-CC 105 0 }
Run-Trial 'driftlow-min' { Send-CC 106 0 }
Run-Trial 'driftlow-max' { Send-CC 106 127 }
Run-Trial 'drifthigh-tight' { Send-CC 107 0 }
Run-Trial 'drifthigh-loose' { Send-CC 107 127 }
Run-Trial 'all-off-prebypass-snap-match' { Send-CC 103 127; Send-CC 104 0; Send-CC 105 0 }

$results | Set-Content "$root\results.txt"
Write-Host ""
Write-Host "outdir: $root"
Write-Host ""
$results | Where-Object { $_ -match 'cuts=(\d+)' } | Sort-Object { [int]($Matches[1]) }
