# Build and verification

## Current build

`build-phase9.ps1` copies the current sources into a fresh output directory, creates normal and debug UEFI images plus the AOSFS data disk, and runs all host tests in each build. It refuses to overwrite an existing output directory.

```powershell
.\build-phase9.ps1 -OutputDirectory 'vm\phase9-next'
```

Latest completed result: `vm\phase9-mmio-uncached-final` built successfully, with **18/18 host tests passing in normal and debug configurations**. These tests cover boot/artifact validation, PMM, VM mappings and huge-page cache-attribute updates, heap, ELF/syscalls, block I/O contracts, AOSFS, lock checks, and generated disk structure.

## Guest verification

The latest Phase 9 guest failure was traced to an inherited AHCI BAR identity mapping that was writable but cacheable. The kernel now checks the active PAT configuration, updates the existing mapping to PCD|PWT, safely demotes a huge mapping if necessary, and invalidates the active TLB entry. It refuses AHCI register access if the updated mapping cannot be verified.

Guest verification of this correction is **pending**. A passing host build is not guest evidence. The serial log must show AHCI controller/MMIO initialization, AOSFS mounted, the disk-sourced ELF message, and `SELFTEST PASS: disk-sourced ELF64 ran in ring 3` before Phase 9 storage is considered guest-verified.

To create a new test VM from a fresh build, use the matching `-BuildDirectory` and a new `-VMName` with `vm\run-phase9-vbox.ps1`. The launcher preserves existing VM registrations and refuses to reuse a name or serial-log path.

## Prior kernel milestones

The project has saved guest evidence for the UEFI handoff, exception and timer paths, PMM/VM foundations, shell, and the ring-3 syscall smoke test. The current open verification item is the Phase 9 AHCI/AOSFS/disk-ELF path.