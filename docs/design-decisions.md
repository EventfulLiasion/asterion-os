# Design decisions

## UEFI owns the first-stage load

Asterion remains a PE32+ UEFI application at `EFI/BOOT/BOOTX64.EFI`. The firmware loads the kernel image, provides GOP and the memory map, and transfers control after `ExitBootServices`. Keeping this boundary avoids maintaining a separate first-stage bootloader.

## Preserve and audit firmware state

The kernel validates a versioned handoff, seeds PMM only from UEFI conventional-memory descriptors, clones the complete active page-table tree, and switches CR3 only after critical mappings pass validation. Firmware mappings remain available unless a subsystem deliberately changes a mapping with an explicit safety check.

## Treat device memory separately

AHCI BAR mappings must be identity-mapped, writable, and uncached before controller registers are touched. If firmware supplied a cacheable mapping, the kernel updates only the BAR page, demotes huge mappings when needed, verifies the PAT entries selected for UC, and invalidates the active TLB entry. The GOP framebuffer retains its firmware cache policy because its preferred memory type is a separate decision.

## Keep early diagnostics independent

COM1 logging is polled and bounded so it works before allocation, paging changes, or interrupts. After `ExitBootServices`, fatal diagnostics do not depend on framebuffer initialization.

## Keep current concurrency limits explicit

PMM, VM, heap, and scheduler state are single-core. The timer is a legacy PIT/PIC path; device storage is polled. The kernel has no SMP, blocking synchronization, writable filesystem, or general user process scheduler. New concurrency or storage mechanisms should follow verified behavior of the existing path rather than being introduced preemptively.