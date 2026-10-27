param([switch]$DebugBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { throw 'Python 3 was not found. Install Python 3 and add python.exe to PATH.' }
$pythonPath = $python.Source
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc -and (Test-Path 'C:\msys64\mingw64\bin\gcc.exe')) {
    $gccPath = 'C:\msys64\mingw64\bin\gcc.exe'
} elseif ($gcc) {
    $gccPath = $gcc.Source
} else {
    throw 'GCC was not found. Install the MSYS2 MinGW-w64 x86_64 toolchain.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $root 'EFI\BOOT') | Out-Null
$optimization = if ($DebugBuild) { '-O0' } else { '-O2' }
$gccArgs = @(
    '-std=c17',$optimization,'-Wall','-Wextra','-fno-builtin','-mgeneral-regs-only',
    '-ffreestanding','-fno-stack-protector','-fno-stack-check',
    '-fno-asynchronous-unwind-tables','-fno-unwind-tables','-mno-red-zone',
    '-nostdlib','-Wl,--subsystem,10','-Wl,-e,efi_main','-Wl,--enable-reloc-section','-Wl,--dynamicbase',
    '-I',(Join-Path $root 'include'),
    '-o',(Join-Path $root 'EFI\BOOT\BOOTX64.EFI'),
    (Join-Path $root 'kernel.c'),
    (Join-Path $root 'drivers\serial.c'),
    (Join-Path $root 'drivers\keyboard.c'),
    (Join-Path $root 'drivers\framebuffer.c'),
    (Join-Path $root 'arch\x86_64\cpu.c'),
    (Join-Path $root 'arch\x86_64\interrupts.c'),
    (Join-Path $root 'arch\x86_64\interrupts.S'),
    (Join-Path $root 'boot\validate.c'),
    (Join-Path $root 'boot\uefi.c'),
    (Join-Path $root 'kernel\calc.c'),
    (Join-Path $root 'kernel\block_core.c'),
    (Join-Path $root 'kernel\fs_core.c'),
    (Join-Path $root 'kernel\sync_core.c'),
    (Join-Path $root 'kernel\sync.c'),
    (Join-Path $root 'kernel\ahci.c'),
    (Join-Path $root 'kernel\heap_core.c'),
    (Join-Path $root 'kernel\heap.c'),
    (Join-Path $root 'kernel\sched_core.c'),
    (Join-Path $root 'kernel\threads.c'),
    (Join-Path $root 'kernel\process.c'),
    (Join-Path $root 'kernel\elf64_loader.c'),
    (Join-Path $root 'kernel\elf64_vm.c'),
    (Join-Path $root 'kernel\hello_elf_blob.c'),
    (Join-Path $root 'kernel\syscall_core.c'),
    (Join-Path $root 'kernel\user_test.S'),
    (Join-Path $root 'kernel\pmm.c'),
    (Join-Path $root 'kernel\vm_core.c'),
    (Join-Path $root 'kernel\vm.c')
)
if ($DebugBuild) { $gccArgs += @('-g3','-DDEBUG','-fno-omit-frame-pointer') }
& $pythonPath (Join-Path $root 'user\build-hello-elf.py')
if ($LASTEXITCODE -ne 0) { throw "Standalone user ELF build failed with exit code $LASTEXITCODE" }
& $gccPath @gccArgs
if ($LASTEXITCODE -ne 0) { throw "Kernel build failed with exit code $LASTEXITCODE" }
& $pythonPath (Join-Path $root 'make-image.py')
if ($LASTEXITCODE -ne 0) { throw "Disk image creation failed with exit code $LASTEXITCODE" }
& $pythonPath (Join-Path $root 'make-uefi-disk.py')
if ($LASTEXITCODE -ne 0) { throw "UEFI hard-disk image creation failed with exit code $LASTEXITCODE" }
& $pythonPath (Join-Path $root 'make-phase9-data-disk.py')
if ($LASTEXITCODE -ne 0) { throw "AOSFS data disk creation failed with exit code $LASTEXITCODE" }
Write-Host 'Asterion OS is ready. The GPT/FAT32 boot VHD and separate AOSFS data VHD are available; use the matching launcher for storage tests.' -ForegroundColor Green

