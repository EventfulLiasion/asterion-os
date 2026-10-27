# Contributing

Asterion is a freestanding x86-64 UEFI kernel. Keep changes small and preserve the UEFI boot path, interrupt entry/return ABI, memory isolation, and existing serial diagnostics.

## Build and host tests

Use Windows PowerShell with the MSYS2 MinGW-w64 x86-64 GCC toolchain and Python 3 available. Build into a fresh ignored directory so generated images and VM state stay out of source control:

```powershell
.\build-phase9.ps1 -OutputDirectory 'vm\contributor-build'
```

This produces normal and debug images and runs the complete host test suite for both. The script refuses to overwrite an existing output directory. Use another directory name for a later build.

## Guest verification

Host tests do not establish guest behavior. For storage changes, use a separate VirtualBox VM and verify its serial log shows AHCI initialization, AOSFS mount, and the disk-sourced ELF self-test PASS line. Never overwrite a known-good VM or claim guest success without serial evidence.

Do not commit generated EFI/VHD/FAT images, VirtualBox configuration/state, serial logs, or compiler outputs. The root `.gitignore` excludes these artifacts.