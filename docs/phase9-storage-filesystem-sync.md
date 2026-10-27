# Storage and disk ELF smoke test

Asterion's current storage path consists of a polled AHCI driver, a bounded block-device read interface, and a read-only fixed-layout AOSFS v1 volume. The formatter creates a separate 16 MiB data VHD containing `hello.elf`; the UEFI boot VHD remains separate.

AHCI uses a PCI bus-0 controller, a 32-bit BAR5, ATA IDENTIFY, LBA48 READ DMA EXT, 32-bit DMA addresses, and one polled command slot. The boot smoke path mounts AOSFS, reads and checksum-validates `hello.elf`, and runs it through the existing isolated ELF loader and ring-3 syscall path. `FS` lists mounted entries. Without a supported data disk, the embedded ELF smoke path remains available.

The AHCI BAR path requires a writable identity mapping with PCD|PWT. If UEFI left a cacheable mapping, the VM core now updates that mapping safely, demotes 1 GiB/2 MiB leaves as needed, verifies that the selected PAT entries mean UC, and invalidates the TLB entry. It does not access controller registers until those checks pass. The framebuffer retains its firmware-provided cache policy.

## Build and launch

Build into a new directory; the script refuses to overwrite previous output:

```powershell
.\build-phase9.ps1 -OutputDirectory 'vm\phase9-next'
.\vm\run-phase9-vbox.ps1 -BuildDirectory 'phase9-next\normal' -VMName 'AsterionOS-Phase9-Next'
```

The launcher creates a separate EFI64 VM, attaches the boot VHD at SATA port 0 and data VHD at port 1, and captures COM1 in `vm\phase9-next\normal\kernel-serial.log`. It checks for an existing VM registration and refuses to overwrite a VM folder or serial log.

## Verification and limits

Normal and debug builds currently pass **18/18 host tests each**. They validate generated images, filesystem metadata/checksums, block bounds, VM cache-attribute updates, and ELF loading logic; they do not emulate AHCI, DMA, or VirtualBox disk execution.

Guest verification of the latest AHCI cache fix is pending. Require the serial log to show successful AHCI initialization, `AHCI SATA block device mounted read-only AOSFS`, the `hello.elf` syscall output, and `SELFTEST PASS: disk-sourced ELF64 ran in ring 3`. Do not infer a boot from image checks or VM configuration alone.

The storage implementation is single-core, read-only, polling-only, limited to one supported SATA controller, 512-byte sectors, and DMA below 4 GiB. It has no NCQ, ATAPI, hotplug, controller interrupts, writable filesystem, general process lifecycle, or blocking lock primitives.