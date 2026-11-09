# Does Windows itself see the Xbox Elite Controller as an XInput gamepad?
#
# WHY: Eden binds every guest controller through its own input engine. In this
# install every player binding is "engine:keyboard", so no host pad reaches the
# guest. Before telling the user "configure Eden", we must know the pad is even
# alive on this PC: if Windows/XInput cannot see it, no Eden setting can help.
# XInput is also the backend SDL2 (Eden's Windows gamepad engine) sits on top of.
param(
    [int]$PollSeconds = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class XInputProbe {
    [StructLayout(LayoutKind.Sequential)]
    public struct Gamepad {
        public ushort Buttons;
        public byte LeftTrigger;
        public byte RightTrigger;
        public short ThumbLX;
        public short ThumbLY;
        public short ThumbRX;
        public short ThumbRY;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct State {
        public uint PacketNumber;
        public Gamepad Gamepad;
    }
    [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
    static extern uint GetState(uint index, out State state);
    public static uint[] Slots() {
        var r = new uint[4];
        for (uint i = 0; i < 4; i++) {
            State s;
            r[i] = GetState(i, out s);
        }
        return r;
    }
    public static uint Read(uint index, out State state) { return GetState(index, out state); }
}
"@

$names = @('LEFT', 'RIGHT', 'UP', 'DOWN', 'START', 'BACK', 'LS', 'RS',
           'A', 'B', 'X', 'Y', 'LSTICK', 'RSTICK', 'ZL', 'ZR')

function Format-State([XInputProbe.State]$st) {
    $pressed = @()
    for ($i = 0; $i -lt 16; $i++) {
        if (($st.Gamepad.Buttons -band (1 -shl $i)) -ne 0) { $pressed += $names[$i] }
    }
    $lx = $st.Gamepad.ThumbLX
    $ly = $st.Gamepad.ThumbLY
    $rx = $st.Gamepad.ThumbRX
    $ry = $st.Gamepad.ThumbRY
    return ("buttons=[{0}] lt={1} rt={2} lx={3} ly={4} rx={5} ry={6}" -f
        ($pressed -join '+'), $st.Gamepad.LeftTrigger, $st.Gamepad.RightTrigger, $lx, $ly, $rx, $ry)
}

Write-Output "XINPUT-SLOT-SCAN"
$slots = [XInputProbe]::Slots()
for ($i = 0; $i -lt 4; $i++) {
    $code = $slots[$i]
    $txt = switch ($code) {
        0 { 'CONNECTED' } 1 { 'ABSENT' } 2 { 'ERROR' } default { "CODE$code" }
    }
    Write-Output ("  slot{0} {1}" -f $i, $txt)
}

if ($PollSeconds -gt 0) {
    Write-Output ("XINPUT-POLL {0}s" -f $PollSeconds)
    $deadline = (Get-Date).AddSeconds($PollSeconds)
    while ((Get-Date) -lt $deadline) {
        for ($i = 0; $i -lt 4; $i++) {
            $st = New-Object XInputProbe+State
            if ([XInputProbe]::Read([uint32]$i, [ref]$st) -eq 0) {
                Write-Output ("  t={0:HH:mm:ss} slot{1} {2}" -f (Get-Date), $i, (Format-State $st))
            }
        }
        Start-Sleep -Milliseconds 250
    }
}
