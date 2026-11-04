# Turn the guest-thread census off (or back on) on the emulator card.
#
# WHY: the census writes ~250 lines per 4-second sweep, and it SAMPLES the
# guest by PAUSING ITS THREADS -- MEASURED (q138): "phase=sampler-flush ...
# phase=in-guest control_in_guest=0/7071", i.e. 7,071 guest threads were
# paused and none of them had been resumed.  The port's own watchdog says the
# same thing ("sampler_last_tick=9", "the sampler thread stopped").
#
# The port's frame cost is measured at 942.8 s of CPU for 604 s of wall clock,
# which is 413 ms of CPU PER FRAME (2280 frames) -- 60 fps would need 25 ms.
# Eden's own renderer is on the host GPU (NVIDIA, Vulkan), so that cost is the
# PORT's, and the census is the part of the port that touches the guest's
# threads every frame.
#
# The kill switch already exists in the port: "census=off" in
# sdmc:/switch/rr3-census.conf, or RR3_CENSUS=0.  The file is ABSENT on the
# card, which means the census is ARMED.
#
# Usage: powershell -File .build\deploy-census-conf.ps1 [-Off]

param(
    [switch]$Off
)

$ErrorActionPreference = 'Stop'

$card = 'E:\Eden-rr3\user\sdmc\switch\rr3-census.conf'

if ($Off) {
    $text = "census=off`n"
}
else {
    $text = "census=on`n"
}

[System.IO.File]::WriteAllText($card, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("wrote {0} -> {1}" -f $card, $text.Trim())
