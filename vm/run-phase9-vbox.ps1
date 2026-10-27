param([string]$BuildDirectory = 'phase9-build\normal', [string]$VMName = 'AsterionOS-Phase9')
$ErrorActionPreference = 'Stop'
$vbox = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
$vmName = $VMName
$vmBase = $PSScriptRoot
$build = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $BuildDirectory))
$bootDisk = Join-Path $build 'asterion-uefi.vhd'
$dataDisk = Join-Path $build 'asterion-data.vhd'
$vmFolder = Join-Path $PSScriptRoot $vmName
$log = Join-Path $build 'kernel-serial.log'
if (-not (Test-Path -LiteralPath $vbox)) { throw "VBoxManage not found: $vbox" }
foreach ($path in @($bootDisk, $dataDisk)) { if (-not (Test-Path -LiteralPath $path)) { throw "Required Phase 9 disk not found: $path" } }
if ((Get-Item -LiteralPath $bootDisk).Length -ne (64MB + 512)) { throw 'Phase 9 boot VHD has an unexpected size.' }
if ((Get-Item -LiteralPath $dataDisk).Length -ne (16MB + 512)) { throw 'Phase 9 data VHD has an unexpected size.' }
if (Test-Path -LiteralPath $vmFolder) { throw "VM folder already exists; inspect it before proceeding: $vmFolder" }
if (Test-Path -LiteralPath $log) { throw "Serial log already exists; preserve it and move it manually before rerunning: $log" }
$registered = @(& $vbox list vms 2>&1)
if ($LASTEXITCODE -ne 0) { throw "Cannot inspect registered VMs; no changes were made: $($registered -join ' ')" }
if ($registered | Where-Object { "$_" -match ('^"' + [regex]::Escape($vmName) + '"\s+\{') }) { throw "VM '$vmName' already exists; no existing registration was changed." }
$running = @(& $vbox list runningvms 2>&1)
if ($LASTEXITCODE -ne 0) { throw "Cannot inspect running VMs; no changes were made: $($running -join ' ')" }
& $vbox createvm --name $vmName --ostype Other_64 --basefolder $vmBase --register
if ($LASTEXITCODE -ne 0) { throw 'VM creation failed; inspect VirtualBox for a partial registration before retrying.' }
& $vbox modifyvm $vmName --firmware=efi64 --memory=256 --vram=32 --graphicscontroller=vboxsvga --keyboard=ps2 --nic1=none --audio-enabled=off --boot1=disk --boot2=none --boot3=none --boot4=none --uart1 0x3F8 4 --uart-mode1 file $log
if ($LASTEXITCODE -ne 0) { throw 'VM configuration failed; preserve and inspect the partial VM.' }
& $vbox storagectl $vmName --name=SATA --add=sata --controller=IntelAhci --bootable=on
if ($LASTEXITCODE -ne 0) { throw 'SATA configuration failed; preserve and inspect the partial VM.' }
& $vbox storageattach $vmName --storagectl=SATA --port=0 --device=0 --type=hdd "--medium=$bootDisk"
if ($LASTEXITCODE -ne 0) { throw 'Boot disk attachment failed; preserve and inspect the partial VM.' }
& $vbox storageattach $vmName --storagectl=SATA --port=1 --device=0 --type=hdd "--medium=$dataDisk"
if ($LASTEXITCODE -ne 0) { throw 'Data disk attachment failed; preserve and inspect the partial VM.' }
$info = & $vbox showvminfo $vmName --machinereadable 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify VM configuration; VM was not started.' }
$normalized = $info.Replace('\\', '\')
$bootAttachment = [regex]::Escape('"SATA-0-0"="' + $bootDisk + '"')
$dataAttachment = [regex]::Escape('"SATA-1-0"="' + $dataDisk + '"')
$serialPath = [regex]::Escape($log)
if ($normalized -notmatch 'firmware="EFI(64)?"' -or $normalized -notmatch $bootAttachment -or $normalized -notmatch $dataAttachment -or $normalized -notmatch 'uart1="0x3f8,4"' -or $normalized -notmatch 'uartmode1="file"' -or $normalized -notmatch $serialPath) { throw 'UEFI, exact disk ports, or serial configuration verification failed; VM was not started.' }Write-Host "Verified separate UEFI VM with boot VHD at SATA 0 and AOSFS data VHD at SATA 1. Serial log: $log"
& $vbox startvm $vmName --type headless
if ($LASTEXITCODE -ne 0) { throw 'VirtualBox did not start the guest; preserve VM and inspect its logs.' }
Write-Host 'VM start requested. Inspect the serial log for AHCI/AOSFS and disk ELF PASS evidence.'
