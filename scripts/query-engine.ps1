# query-engine.ps1 — talk to the Looper on-demand observability/control surface
# on the Pi's debug socket (UDP 192.168.137.100:4445).
#
#   .\query-engine.ps1                 # GET full engine snapshot
#   .\query-engine.ps1 "GET"           # same
#   .\query-engine.ps1 "SET 1 0.5"     # set live param (id 1 = grain factor)
#   .\query-engine.ps1 -Sweep          # engage -12 then sweep formant + GET each
#
# SET ids: 0 formantDepth, 1 grainFactor(direct), 2 grainMix(direct),
#          3 readOffset, 4 xfadeScale, 5 fidelity, 6 preBypass
param([string]$Msg = "GET", [switch]$Sweep, [string]$Ip = "192.168.137.100")

function Q($m) {
    $c = New-Object System.Net.Sockets.UdpClient
    $c.Connect($Ip, 4445)
    $b = [System.Text.Encoding]::ASCII.GetBytes($m); $c.Send($b, $b.Length) | Out-Null
    $c.Client.ReceiveTimeout = 2000
    try { $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any,0); $r = $c.Receive([ref]$ep); $s = [System.Text.Encoding]::ASCII.GetString($r) }
    catch { $s = "<no reply>" }
    $c.Close(); return $s
}
function Midi($arr) { $m = New-Object System.Net.Sockets.UdpClient; $m.Send($arr, $arr.Length, $Ip, 4446) | Out-Null; $m.Close() }

if ($Sweep) {
    "baseline:   " + (Q "GET")
    Midi @(0x92,48,127); Start-Sleep -Milliseconds 500       # engage -12
    "engaged-12: " + (Q "GET")
    foreach ($cc in 0,32,64,96,127) {
        Midi @(0xB0,53,$cc); Start-Sleep -Milliseconds 500
        ("CC53=$cc:   ") + (Q "GET")
    }
    # also try driving the grain factor DIRECTLY (bypasses the depth/CC path)
    foreach ($f in 0.5,1.0,1.5,2.0) {
        Q ("SET 1 " + $f) | Out-Null; Start-Sleep -Milliseconds 500
        ("SET fac=$f: ") + (Q "GET")
    }
} else {
    Q $Msg
}
