# Asterion architecture (implemented state)

## Boot contract

Firmware loads `EFI/BOOT/BOOTX64.EFI` as an x86-64 PE32+ EFI application. `efi_main` discovers GOP, validates its pixel format and geometry, records CPU metadata, obtains the memory map, reserves loader-data storage for the map, then exits boot services. `kernel_main` receives a version-1 `boot_info_t` containing framebuffer range/stride/format, the map pointer and descriptor stride/count, conventional-memory total, and CPUID data.

The map is a firmware snapshot used to seed the physical allocator after exit; it is not itself an allocator. Its descriptors use the 40-byte UEFI x64 prefix. Descriptor iteration uses the firmware-provided descriptor size as a stride. The handoff buffer is allocated as `EfiLoaderData` before exit and remains reserved while the kernel reads it. Kernel globals, code, and stack are part of the loaded EFI image/runtime allocation and are not reclaimed. The kernel validates the 168-byte, 8-byte-aligned v1 boot record, terminated CPU strings, map pointer/length/stride/count, descriptor page ranges, and recomputed conventional-memory total before touching the framebuffer.

Firmware calls use the Microsoft x64 ABI. The current UEFI structure definitions include only fields needed by the boot path; boot-service slots are indexed from the UEFI table header. Descriptor version 1 is required. Boot-services functions are not called after successful `ExitBootServices`; a stale-key `EFI_INVALID_PARAMETER` is retried while other errors stop the handoff. Interrupts are disabled immediately after the successful exit and remain disabled while the kernel validates its handoff and installs its own CPU tables. The framebuffer range is provided by firmware and remains mapped when the kernel clones the active page tables. The kernel preserves its firmware cache policy.

## VirtualBox boot media

The supported VirtualBox UEFI boot path uses a GPT disk with a FAT32 EFI System Partition containing `EFI/BOOT/BOOTX64.EFI`. The 1.44 MB FAT12 image is retained for removable-media and structural checks; the saved VirtualBox UEFI configuration did not boot it reliably.

## Physical frame allocator

`kernel/pmm.c` uses a statically reserved two-bit state map for up to 16 GiB of physical address space (4 KiB frames): reserved, free, or allocated. The bitmap resides in the EFI-loaded kernel image and is consequently part of a non-conventional loader allocation. Initialization admits only `EfiConventionalMemory`; every other UEFI descriptor type stays reserved. Contradictory overlapping conventional descriptors cause initialization to fail. The first physical page is withheld, and the framebuffer's page-rounded range is explicitly reserved even if a firmware map labels it conventional. The kernel image, its stack and globals (including the copied handoff and allocator metadata), and retained memory-map buffer remain in firmware loader allocations, not allocator candidates.

Allocation supports contiguous page runs, power-of-two page alignment, and an exclusive maximum physical address. Free validates the complete range before changing any frame, so partial frees, reserved-frame frees, out-of-range frees, and double frees are rejected without modifying state. `MEM` reports conventional, free, allocated, and reserved conventional frames. Boot-time self-tests allocate and release aligned pages and reject invalid frees. Host tests also cover exhaustion and bounds.

The allocator only returns physical addresses. It does not establish virtual mappings, zero pages, or allow those addresses to be dereferenced; those require the later virtual-memory phase. The implementation is single-CPU and callers must not use it concurrently from interrupt contexts. Physical addresses above 16 GiB are intentionally left unmanaged.

## Virtual memory and inherited layout

The EFI artifact is PE32+ with preferred image base `0x140000000` and `SizeOfImage` `0x143000` in the Phase 4 build. Its section layout is `.text` RVA `0x1000`, size `0x7D80`; `.data` RVA `0x9000`, size `0x130`; `.rdata` RVA `0xA000`, size `0x2180`; `.bss` RVA `0xD000`, size `0x133DE0`; `.idata` RVA `0x141000`; and `.reloc` RVA `0x142000`. Firmware may relocate the image, so these are preferred addresses. `.bss` contains the kernel state, PMM map, page-table tracking, interrupt stacks, and handoff copy. The active stack is supplied by firmware and its runtime address is read during VM initialization.

UEFI enters the kernel with paging already enabled. `vm_init` reads CR0, CR3, CR4, and EFER, detects four- versus five-level paging and physical-address width, and walks the actual active root before changing it. The saved Phase 4 serial log records four-level paging with inherited CR3 `0x0EE01000`; kernel code at `0x0D2CF0D0`, active stack at `0x0F0F7680`, handoff at `0x0D2D4000`, retained memory map at `0x0D44F018`, framebuffer at `0x80000000`, and old CR3 page were identity-translated. The cloned root was physical `0x1000`. In this logger's flags, `0x3` means write+execute and was reported for code, stack, handoff, map, and framebuffer; `0x2` means read/execute without write and was reported for the old CR3 page. This guest therefore confirms that the inherited firmware map is not W^X. These are observations from that VirtualBox run, not universal UEFI address guarantees.

Startup requires the active root and required PMM table frames to be readable through their physical/identity addresses, and checks writable access before using a conventional frame as a page table. It recursively clones every present non-large page-table page into zeroed PMM frames, preserving leaf entries and large-page mappings. It verifies the required live mappings in the clone before switching CR3; failures before the switch leave UEFI's root active, while post-switch validation failure restores the old CR3. This keeps the full firmware mapping set, including executing code, stack, framebuffer, descriptor data, interrupt tables, and stacks. The old firmware tables remain reserved and are not reclaimed.

`vm_core.c` implements four- and five-level canonical-address checks, physical-width validation, large-page translation, effective permission reporting, conflict checks, and transactional table-page allocation for 4 KiB mappings. `vm_map_page`, `vm_unmap_page`, and `vm_translate_address` operate on the kernel-owned root. Unmap only accepts mappings created through this API, protecting inherited UEFI mappings; it leaves empty table pages allocated. Mapping invalidates the target TLB entry. Managed mappings support read (implicit), write, supervisor/user, execute/NX, global, and cache flags. The mapper rejects write+execute combinations for new mappings. Existing UEFI entries are preserved during VM initialization. The explicit cache-attribute updater can change one inherited mapping, demoting 1 GiB or 2 MiB leaves when needed and invalidating the active TLB entry; Phase 9 uses it only for AHCI BAR pages. Other inherited mappings, including the firmware framebuffer, retain firmware cache attributes. This is not a system-wide W^X guarantee.

At boot, a test page is mapped to an allocated PMM frame, translated, written and read, unmapped, then read through a controlled #PF recovery path. If NX is available, a harmless `RET` byte is placed in a writable/NX test page and an instruction-fetch #PF is expected. Saved guest output confirms the CR3 transition and controlled fault tests. Host tests cover entry encoding, canonical addresses, translations, effective permissions, huge-page lookup and cache demotion, conflicts, unmapping, and allocation failure.

The implementation assumes a single CPU; mappings and the PMM are not synchronized for concurrent access from interrupt contexts. Page-table pages are not reclaimed after unmapping. On firmware that does not identity-map active table pages and PMM frames, initialization fails safely before the CR3 switch rather than attempting an incomplete root.

## Kernel heap

The kernel heap occupies the lower-canonical interval `[0x0000500000000000, 0x0000500000040000)`. Startup verifies every 4 KiB page is unmapped, obtains 64 physical frames from PMM, maps them through the active kernel VM root as supervisor read/write and NX when supported, and zeroes the arena only through its new virtual mapping. A failed mapping rolls back the mapped backing pages and allocated data frames before heap metadata is initialized. This region is far from the observed low identity-mapped EFI image/stack/handoff and framebuffer at `0x80000000`; the VM conflict check is the safety guard.

`heap_core.c` is a host-testable first-fit allocator with 16-byte alignment, in-arena block metadata, splitting, adjacent-block coalescing, zero-size rejection, overflow checks, exhaustion behavior, and exact-pointer validation before freeing. `kmalloc` returns null on failure; `kfree` returns zero for null, foreign/interior, or already-freed pointers. The arena is fixed at 256 KiB and is not expanded; mapped data frames remain reserved until shutdown. Heap state is single-core and not safe for concurrent use; interrupt handlers must not allocate or free.

## Kernel threads and scheduler

The kernel has one permanent boot-shell/idle thread and up to 15 heap-stack-backed worker threads. A FIFO ready queue is switched from the common interrupt epilogue using complete interrupt frames; PIT IRQ0 preempts workers and software vector 0x30 performs voluntary yield/termination. The normalized x86-64 frame consumed by IRETQ is 176 bytes, including the hardware-pushed SS:RSP words even for same-CPL interrupts. Synthetic thread frames initialize those words explicitly. Stacks are reclaimed later by the shell after a terminated thread is no longer current. The build forbids general-register SIMD use and the switch does not save floating-point/SIMD state. This remains a single-CPU ring-0 scheduler with no blocking/wakeup primitives.

## Current memory assumptions

- The kernel retains all mappings found in the firmware's active page-table tree when it builds its own root.
- `framebuffer_base` must be nonzero; `base + size` must not overflow; the visible `stride * height * 4` byte range must fit the firmware's reported buffer.
- The memory-map pointer must remain readable after exit; it is retained in a firmware loader allocation and read-only after exit.
- Each memory descriptor must have a page-aligned base, nonzero page count, and nonoverflowing end address.
- Memory-map growth is bounded and `ExitBootServices` is retried when firmware rejects a stale map key.
- The PE data relocation anchor validates that one image-relative pointer was adjusted by the loader. It does not validate every firmware's loader behavior.

## CPU tables, exceptions, and timer IRQ

`arch/x86_64/interrupts.c` installs a minimal ring-0 GDT, a 64-bit TSS with dedicated privilege-transition, double-fault, and page-fault stacks, and a 256-entry IDT. `interrupts.S` provides normalized exception frames and saves all general-purpose registers before dispatching to C. The C dispatcher recovers only the boot-time expected #UD, #GP, and #PF tests; unexpected CPU exceptions log vector, error code, saved RIP, and CR2 for page faults over COM1, then halt. Other PIC interrupts are masked after the 8259 is remapped to vectors 32–47; IRQ0 is the only enabled hardware line. The PIT is programmed near 100 Hz, and its handler increments a volatile tick count and sends PIC EOI. Vector 255 is installed for local-APIC spurious interrupts and returns without EOI. C is built with `-mgeneral-regs-only` so the interrupt dispatcher cannot corrupt interrupted SIMD state.

This subsystem is implemented and guest-verified; captured serial output contains the exception recovery, CR2, and PIT timer pass messages. See [testing.md](testing.md).

## Console and input

`drivers/serial.c` owns COM1 initialization, bounded character writes, and level-prefixed logging. `drivers/framebuffer.c` owns the validated GOP text console and its scrollback; `drivers/keyboard.c` offers a nonblocking i8042 poll and bounded reset request. `kernel/calc.c` is the checked signed 64-bit expression evaluator. `boot/uefi.c` owns the firmware entry and exit path, while `kernel.c` still owns shell commands and kernel startup. CPU identification is isolated in `arch/x86_64/cpu.c`. An absent keyboard controller is reported and polling continues without a busy wait. Saved serial output verifies the boot diagnostics, exception/timer tests, PMM/VM foundations, and shell. Phase 9 storage and disk-ELF behavior still need a fresh guest run.

## Boot sequence diagram

```mermaid
sequenceDiagram
    participant U as UEFI
    participant E as efi_main
    participant G as GOP
    participant M as Memory Map
    participant K as kernel_main
    U->>E: load PE32+ EFI application
    E->>G: LocateProtocol; validate current mode
    E->>M: size map and allocate EfiLoaderData buffer
    loop bounded handoff attempts
      E->>M: GetMemoryMap(buffer)
      E->>U: ExitBootServices(image, map_key)
    end
    U-->>K: successful exit; boot_info_t v1
    K->>K: disable interrupts; initialize console and GDT/TSS/IDT
    K->>K: recover controlled #UD/#GP/#PF self-tests
    K->>K: remap PIC, start PIT IRQ0, enable interrupts
    K->>K: initialize PMM from retained UEFI memory map
    K->>K: audit and clone inherited page tables
    K->>K: switch CR3 only after preserved-mapping checks
    K->>K: run VM mapping, unmapping, and fault self-tests
    K->>K: map fixed heap from PMM frames; run heap self-tests
    K->>K: print diagnostics and poll shell
```

## Current limits

The ring-3 path is a synchronous architecture smoke test, not a general process scheduler. User CS/SS selectors are `0x2B`/`0x33`; the kernel uses a TSS-provided entry stack, contained user faults, and syscall ABI v1 on vector `0x32` (`write`, `exit`, `yield`, `getpid`). User mappings are audited for supervisor isolation, while inherited firmware mappings remain shared and supervisor-only.

The scheduler and memory managers are single-core. The kernel heap is fixed-size. AOSFS is read-only; AHCI is polled, single-controller, and limited to 32-bit DMA. No SMP, network stack, writable filesystem, general user process lifecycle, or blocking synchronization is implemented. Phase 9 guest storage verification remains pending; see [testing.md](testing.md).