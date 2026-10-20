<#
.SYNOPSIS
  Read-only checks for the PS5 Input Bridge latency setup. Needs no administrator rights.
.PARAMETER Check
  Settings   - wired adapter, power plan and USB power settings
  Traffic    - bytes the PC receives on the wired adapter for -Seconds (run while connected to the PS5)
  Controller - real XInput report rate while you move a stick for -Seconds
#>
param([ValidateSet('Settings', 'Traffic', 'Controller')][string]$Check = 'Settings',
      [int]$Seconds = 10, [int]$ControllerIndex = 0, [string]$AdapterName)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

function Get-WiredAdapter {
    if ($AdapterName) { return Get-NetAdapter -Name $AdapterName }
    $candidates = @(Get-NetAdapter | Where-Object {
        $_.Status -eq 'Up' -and $_.MediaType -eq '802.3' -and -not $_.Virtual -and
        $_.InterfaceDescription -notmatch 'TAP|VPN|Virtual|Hyper-V|WireGuard|Tailscale|ZeroTier' })
    if ($candidates.Count -ne 1) { throw "找到 $($candidates.Count) 个有线网卡，请用 -AdapterName 指定。" }
    return $candidates[0]
}

switch ($Check) {
'Settings' {
    $adapter = Get-WiredAdapter
    Write-Output "网卡：$($adapter.Name)（$($adapter.InterfaceDescription)），$($adapter.LinkSpeed)"
    $wanted = [ordered]@{ '*InterruptModeration' = '中断调整'; '*FlowControl' = '流控制'; '*EEE' = '节能以太网';
        'AdvancedEEE' = 'Advanced EEE'; 'EnableGreenEthernet' = '环保节能'; 'GigaLite' = 'Gigabit Lite'; 'PowerSavingMode' = 'Power Saving Mode' }
    $properties = @{}
    Get-NetAdapterAdvancedProperty -Name $adapter.Name -AllProperties | ForEach-Object { $properties[$_.RegistryKeyword] = $_ }
    $pending = 0
    foreach ($keyword in $wanted.Keys) {
        if (-not $properties.ContainsKey($keyword)) { continue }
        $ok = "$($properties[$keyword].RegistryValue)" -eq '0'
        if (-not $ok) { $pending++ }
        Write-Output ("  {0} {1}：{2}" -f ($(if ($ok) { 'OK ' } else { '待改' })), $wanted[$keyword], $properties[$keyword].DisplayValue)
    }
    $scheme = (powercfg /getactivescheme) -join ''
    $ultimate = $scheme -match '卓越性能|Ultimate Performance'
    if (-not $ultimate) { $pending++ }
    Write-Output ("  {0} 电源计划：{1}" -f ($(if ($ultimate) { 'OK ' } else { '待改' })), ($scheme -replace '.*\((.*)\).*', '$1'))
    $usb = ((powercfg /query SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 |
        Select-String '(AC|交流).*0x([0-9a-f]+)').Matches.Groups[2].Value)
    $usbOk = $usb -and [Convert]::ToInt32($usb, 16) -eq 0
    if (-not $usbOk) { $pending++ }
    Write-Output ("  {0} USB 选择性暂停：{1}" -f ($(if ($usbOk) { 'OK ' } else { '待改' })), ($(if ($usbOk) { '已禁用' } else { '已启用' })))
    Write-Output $(if ($pending -eq 0) { '结论：调优已全部生效。' } else { "结论：还有 $pending 项未调优。" })
}
'Traffic' {
    $adapter = Get-WiredAdapter
    $before = Get-NetAdapterStatistics -Name $adapter.Name
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $Seconds
    $after = Get-NetAdapterStatistics -Name $adapter.Name
    $elapsed = $watch.Elapsed.TotalSeconds
    $bytes = $after.ReceivedBytes - $before.ReceivedBytes
    $packets = ($after.ReceivedUnicastPackets - $before.ReceivedUnicastPackets)
    $kbps = $bytes * 8 / 1000 / $elapsed
    Write-Output ("{0:N1} 秒内网卡接收：{1:N0} 字节，{2:N0} 个单播包 → {3:N0} kbit/s，{4:N0} 包/秒" -f $elapsed, $bytes, $packets, $kbps, ($packets / $elapsed))
    Write-Output '说明：这是整张网卡的接收量，包含电脑上其他程序的流量。请分别在“已连接 PS5”和“已断开”时各测一次对比。'
    Write-Output '      已连接时多出约 2000 kbit/s 以上，说明 PS5 仍在发视频；只多出几十 kbit/s，说明只剩控制和心跳数据。'
}
'Controller' {
    Add-Type -Namespace Probe -Name XInput -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct State { public uint Packet; public ushort Buttons; public byte LT, RT; public short LX, LY, RX, RY; }
[DllImport("xinput1_4.dll")] public static extern uint XInputGetState(uint index, out State state);
'@
    $state = New-Object Probe.XInput+State
    if ([Probe.XInput]::XInputGetState($ControllerIndex, [ref]$state) -ne 0) { throw "XInput $($ControllerIndex + 1) 没有连接手柄。" }
    Write-Output "开始测量 $Seconds 秒：请一直转动摇杆画圈（不要停）。"
    $last = $state.Packet; $changes = 0; $samples = 0
    $intervals = New-Object System.Collections.Generic.List[double]
    $clock = [Diagnostics.Stopwatch]::StartNew(); $lastChange = 0.0
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        [void][Probe.XInput]::XInputGetState($ControllerIndex, [ref]$state); $samples++
        if ($state.Packet -ne $last) {
            $now = $clock.Elapsed.TotalMilliseconds
            if ($changes -gt 0) { $intervals.Add($now - $lastChange) }
            $last = $state.Packet; $lastChange = $now; $changes++
        }
    }
    if ($intervals.Count -lt 20) { throw '状态变化太少，请在测量期间持续转动摇杆。' }
    $sorted = $intervals | Sort-Object
    $median = $sorted[[int]($sorted.Count / 2)]
    $p95 = $sorted[[int]($sorted.Count * 0.95)]
    Write-Output ("读取 {0:N0} 次；手柄状态更新 {1:N0} 次。相邻更新间隔中位数 {2:N2} ms（约 {3:N0} Hz），P95 {4:N2} ms。" -f $samples, $changes, $median, (1000 / $median), $p95)
    Write-Output '说明：摇杆持续运动时，更新间隔约等于手柄的回报周期。1 ms ≈ 1000 Hz，4 ms ≈ 250 Hz，8 ms ≈ 125 Hz。'
}
}
