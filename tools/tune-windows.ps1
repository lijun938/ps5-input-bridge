<#
.SYNOPSIS
  Low-latency Windows/NIC tuning for PS5 Input Bridge, with backup and restore.
.DESCRIPTION
  Run in an elevated PowerShell. Every value changed is first saved to
  %LOCALAPPDATA%\PS5InputBridge\tuning-backup.json; run with -Restore to put them back.
  The wired adapter restarts once when its properties change (a few seconds offline).
#>
param([switch]$Restore, [string]$AdapterName)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '需要以管理员身份运行 PowerShell。'
}

$backupDir = Join-Path $env:LOCALAPPDATA 'PS5InputBridge'
$backupPath = Join-Path $backupDir 'tuning-backup.json'

# Adapter keywords set to 0 (= off for each of these). Missing keywords are skipped.
$nicOff = @('*InterruptModeration', '*FlowControl', '*EEE', 'AdvancedEEE', 'EnableGreenEthernet', 'GigaLite', 'PowerSavingMode')
$usbSubgroup = '2a737441-1930-4402-8d77-b2bebba308a3'
$usbSelectiveSuspend = '48e6b7a6-50f5-4782-a5d4-53bb8f07e226'

function Get-WiredAdapter {
    if ($AdapterName) { return Get-NetAdapter -Name $AdapterName }
    $candidates = @(Get-NetAdapter | Where-Object {
        $_.Status -eq 'Up' -and $_.MediaType -eq '802.3' -and -not $_.Virtual -and
        $_.InterfaceDescription -notmatch 'TAP|VPN|Virtual|Hyper-V|WireGuard|Tailscale|ZeroTier' })
    if ($candidates.Count -ne 1) {
        throw "找到 $($candidates.Count) 个有线网卡，请用 -AdapterName 指定：" + (($candidates | ForEach-Object Name) -join ', ')
    }
    return $candidates[0]
}

function Get-UsbHubPower {
    Get-CimInstance -Namespace root\wmi -ClassName MSPower_DeviceEnable |
        Where-Object { $_.InstanceName -like 'USB\ROOT_HUB*' }
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath $backupPath)) { throw "没有找到备份：$backupPath" }
    $backup = Get-Content -LiteralPath $backupPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($item in $backup.Nic) {
        Set-NetAdapterAdvancedProperty -Name $backup.Adapter -RegistryKeyword $item.Keyword -RegistryValue $item.Value -NoRestart
    }
    Restart-NetAdapter -Name $backup.Adapter
    powercfg /setacvalueindex $backup.TunedScheme $usbSubgroup $usbSelectiveSuspend $backup.UsbSelectiveSuspendAc | Out-Null
    powercfg /setactive $backup.PowerScheme | Out-Null
    foreach ($hub in $backup.UsbHubs) {
        Get-UsbHubPower | Where-Object InstanceName -eq $hub.InstanceName |
            Set-CimInstance -Property @{ Enable = [bool]$hub.Enable }
    }
    Write-Output "已恢复到调整前的设置（备份：$backupPath）。"
    return
}

$adapter = Get-WiredAdapter
$activeScheme = ((powercfg /getactivescheme) -replace '.*?([0-9a-f-]{36}).*', '$1').Trim()
$ultimate = (powercfg /list | Select-String '([0-9a-f-]{36})\s+\((卓越性能|Ultimate Performance)\)' |
    Select-Object -First 1).Matches.Groups[1].Value
$targetScheme = if ($ultimate) { $ultimate } else { $activeScheme }

$current = @{}
Get-NetAdapterAdvancedProperty -Name $adapter.Name -AllProperties |
    ForEach-Object { $current[$_.RegistryKeyword] = $_.RegistryValue }
$nicChanges = @($nicOff | Where-Object { $current.ContainsKey($_) -and "$($current[$_])" -ne '0' })
$usbValue = ((powercfg /query $targetScheme $usbSubgroup $usbSelectiveSuspend | Select-String '(AC|交流).*0x([0-9a-f]+)').Matches.Groups[2].Value)

if (-not (Test-Path -LiteralPath $backupPath)) {
    # Keep the first backup: it holds the original values even if the script is run again.
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    [ordered]@{
        Adapter = $adapter.Name
        Nic = @($nicOff | Where-Object { $current.ContainsKey($_) } |
            ForEach-Object { [ordered]@{ Keyword = $_; Value = "$($current[$_])" } })
        PowerScheme = $activeScheme
        TunedScheme = $targetScheme
        UsbSelectiveSuspendAc = if ($usbValue) { [Convert]::ToInt32($usbValue, 16) } else { 1 }
        UsbHubs = @(Get-UsbHubPower | ForEach-Object { [ordered]@{ InstanceName = $_.InstanceName; Enable = $_.Enable } })
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $backupPath -Encoding UTF8
    Write-Output "原设置已备份：$backupPath"
}

Write-Output "网卡：$($adapter.Name)（$($adapter.InterfaceDescription)）"
foreach ($keyword in $nicChanges) {
    Set-NetAdapterAdvancedProperty -Name $adapter.Name -RegistryKeyword $keyword -RegistryValue 0 -NoRestart
    Write-Output "  关闭 $keyword（原值 $($current[$keyword])）"
}
if ($nicChanges.Count -gt 0) {
    Write-Output '  重启网卡使设置生效（会断网几秒）…'
    Restart-NetAdapter -Name $adapter.Name
} else { Write-Output '  网卡省电与中断调整已经是关闭状态。' }

powercfg /setacvalueindex $targetScheme $usbSubgroup $usbSelectiveSuspend 0 | Out-Null
powercfg /setactive $targetScheme | Out-Null
Write-Output "电源计划：$targetScheme（卓越性能优先），USB 选择性暂停已关闭"

$hubs = @(Get-UsbHubPower | Where-Object Enable)
$hubs | Set-CimInstance -Property @{ Enable = $false }
Write-Output "USB 根集线器“允许关闭此设备以节约电源”：已取消 $($hubs.Count) 个"
Write-Output '完成。恢复原设置：以管理员身份运行本脚本并加 -Restore。'
