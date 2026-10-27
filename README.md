# Asterion OS

Asterion is a freestanding x86-64 UEFI kernel written primarily in C17, with x86-64 assembly for interrupts and privilege transitions. The current source tree includes a read-only AHCI/AOSFS storage path and a disk-sourced ELF smoke test; it is not a web demo.

## Build and run

Requirements: Windows PowerShell, MSYS2 MinGW-w64 x86-64 GCC, and Python 3. Build into a fresh isolated directory; the script creates normal/debug images and runs the host tests for each.

```powershell
.\build-phase9.ps1 -OutputDirectory 'vm\phase9-next'
```

To launch the normal build in a separate VirtualBox VM:

```powershell
.\vm\run-phase9-vbox.ps1 -BuildDirectory 'phase9-next\normal' -VMName 'AsterionOS-Phase9-Next'
```

The launcher refuses an existing VM name, VM folder, or serial-log path. Verify `vm\phase9-next\normal\kernel-serial.log` for AHCI initialization, AOSFS mounting, and the disk-sourced ELF64 self-test PASS line.

The latest host-tested normal/debug artifacts are in `vm\phase9-mmio-uncached-final`. The MMIO cache fix is implemented there; guest verification remains pending.

## Included systems

UEFI boot and versioned handoff; GOP console; COM1 diagnostics; PS/2 keyboard; GDT/TSS/IDT and exception handling; PIC/PIT timer; PMM and x86-64 page tables; fixed kernel heap; single-CPU kernel threads; isolated ring-3 test and syscall ABI v1; ELF64 loader; polled AHCI; read-only AOSFS.

## Current limits

The Phase 9 normal and debug host builds each pass 18 tests. Host tests do not emulate AHCI, DMA, VirtualBox disk attachment, or ring-3 execution from the data disk. Storage is read-only, polling-only, single-controller, 32-bit-DMA, and 512-byte-sector. There is no SMP, network stack, writable filesystem, general user process scheduler, or blocking synchronization.

See [architecture](docs/architecture.md), [testing](docs/testing.md), [Phase 9 storage](docs/phase9-storage-filesystem-sync.md), [design decisions](docs/design-decisions.md), and [contributing](CONTRIBUTING.md).

## License

No license has been selected yet. Choose one before publishing if you want others to have explicit permission to use or modify the code.