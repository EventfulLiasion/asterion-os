param(
    [string]$VMName = 'AsterionOS-Kernel',
    [string]$SerialLogPath
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vbox = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
if (-not (Test-Path $vbox)) { throw 'VirtualBox was not found at its standard install path.' }
$vmName = $VMName
$vmDir = Join-Path $root 'vm'
New-Item -ItemType Directory -Force -Path $vmDir | Out-Null

$registeredVMs = @(& $vbox list vms 2>&1)
if ($LASTEXITCODE -ne 0) { throw "Could not query registered VirtualBox machines: $($registeredVMs -join ' ')" }
$vmNamePattern = '^"' + [regex]::Escape($vmName) + '"\s+\{'
$isRegistered = $registeredVMs | Where-Object { "$_" -match $vmNamePattern }
if (-not $isRegistered) {
    & $vbox createvm --name $vmName --ostype Other_64 --basefolder $vmDir --register
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the VirtualBox machine.' }
}

$info = & $vbox showvminfo $vmName --machinereadable 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the VirtualBox machine.' }
if ($info -match 'VMState="running"') { throw 'Asterion is already running. Close its VM window before starting it again.' }

$build = Join-Path $root 'build.ps1'
& $build
if ($LASTEXITCODE -ne 0) { throw 'Could not build the current kernel images. Ensure the VM is powered off and release any attached image files.' }
$image = Join-Path $root 'asterion-uefi.vhd'
if ([string]::IsNullOrWhiteSpace($SerialLogPath)) { $serialLog = Join-Path $vmDir 'kernel-serial.log' }
else { $serialLog = [System.IO.Path]::GetFullPath($SerialLogPath) }
& $vbox modifyvm $vmName --os-type=Other_64 --firmware=efi64 --memory=256 --vram=32 --graphicscontroller=vboxsvga --keyboard=ps2 --nic1=none --audio-enabled=off --boot1=disk --boot2=none --boot3=none --boot4=none --uart1 0x3F8 4 --uart-mode1 file $serialLog
if ($LASTEXITCODE -ne 0) { throw 'Could not configure the VirtualBox machine.' }

function Find-SataController([string]$machineInfo) {
    $controllerNames = [regex]::Matches($machineInfo, '(?m)^storagecontrollername(\d+)="([^"]+)"\r?$')
    foreach ($match in $controllerNames) {
        $index = $match.Groups[1].Value
        $name = $match.Groups[2].Value
        if ($machineInfo -match ('(?m)^storagecontrollertype' + $index + '="IntelAhci"\r?$')) { return $name }
    }
    return $null
}

$sataController = Find-SataController $info
if (-not $sataController) {
    & $vbox storagectl $vmName --name=SATA --add=sata --controller=IntelAhci --bootable=on
    if ($LASTEXITCODE -ne 0) { throw 'Could not add the virtual SATA controller.' }
    $info = & $vbox showvminfo $vmName --machinereadable 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw 'Could not verify the virtual SATA controller.' }
    $sataController = Find-SataController $info
    if (-not $sataController) { throw 'VirtualBox did not report the configured Intel AHCI SATA controller.' }
}

& $vbox storageattach $vmName "--storagectl=$sataController" --port=0 --device=0 --type=hdd "--medium=$image"
if ($LASTEXITCODE -ne 0) { throw 'Could not attach the Asterion boot disk.' }
Write-Host 'Starting Asterion OS in VirtualBox. Click inside the VM to focus its keyboard; press Right Ctrl to release it.' -ForegroundColor Cyan
& $vbox startvm $vmName --type gui
if ($LASTEXITCODE -ne 0) { throw 'VirtualBox could not start the guest.' }
