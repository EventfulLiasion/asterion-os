import struct
import shutil
import subprocess
import tempfile
import unittest
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EFI = ROOT / "EFI" / "BOOT" / "BOOTX64.EFI"
IMAGE = ROOT / "asterion-uefi.img"
UEFI_DISK = ROOT / "asterion-uefi.vhd"


def u16(data, offset): return struct.unpack_from("<H", data, offset)[0]
def u32(data, offset): return struct.unpack_from("<I", data, offset)[0]

def fs_fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def parse_pe(pe):
    if len(pe) < 0x40 or pe[:2] != b"MZ": raise ValueError("missing DOS header")
    peoff = u32(pe, 0x3C)
    if peoff > len(pe) - 24 or pe[peoff:peoff + 4] != b"PE\0\0": raise ValueError("invalid PE header bounds")
    coff = peoff + 4
    if u16(pe, coff) != 0x8664: raise ValueError("not x86-64")
    section_count, opt_size = u16(pe, coff + 2), u16(pe, coff + 16)
    opt = coff + 20
    if opt_size < 112 or opt + opt_size > len(pe) or u16(pe, opt) != 0x20B: raise ValueError("invalid PE32+ optional header")
    section_table = opt + opt_size
    if section_count > (len(pe) - section_table) // 40: raise ValueError("section table outside file")
    image_size = u32(pe, opt + 56)
    image_base = struct.unpack_from("<Q", pe, opt + 24)[0]
    if not image_size or image_base > 0xFFFFFFFFFFFFFFFF - image_size: raise ValueError("image base range overflows")
    sections = []
    for i in range(section_count):
        sh = section_table + i * 40
        virtual_size, rva, raw_size, raw_ptr = u32(pe, sh + 8), u32(pe, sh + 12), u32(pe, sh + 16), u32(pe, sh + 20)
        if raw_ptr > len(pe) or raw_size > len(pe) - raw_ptr: raise ValueError("section file bounds invalid")
        if rva > image_size or max(virtual_size, raw_size) > image_size - rva: raise ValueError("section image bounds invalid")
        sections.append((rva, virtual_size, raw_size, raw_ptr))
    if opt_size < 112 + 6 * 8 or u32(pe, opt + 108) < 6: raise ValueError("base relocation directory is absent")
    reloc_rva, reloc_size = u32(pe, opt + 112 + 5 * 8), u32(pe, opt + 112 + 5 * 8 + 4)
    if not reloc_rva or reloc_size < 8 or reloc_rva > image_size or reloc_size > image_size - reloc_rva:
        raise ValueError("base relocation directory outside image")
    reloc_file = None
    for rva, virtual_size, raw_size, raw_ptr in sections:
        delta = reloc_rva - rva
        if 0 <= delta <= raw_size and reloc_size <= raw_size - delta:
            reloc_file = raw_ptr + delta
            break
    if reloc_file is None or reloc_file + reloc_size > len(pe): raise ValueError("base relocation directory is not file-backed")
    end, pos = reloc_file + reloc_size, reloc_file
    while pos < end:
        if end - pos < 8: raise ValueError("truncated relocation block")
        page_rva, block_size = struct.unpack_from("<II", pe, pos)
        if page_rva & 0xFFF or page_rva >= image_size: raise ValueError("relocation page outside image")
        if block_size < 8 or block_size & 1 or block_size > end - pos: raise ValueError("invalid relocation block size")
        for off in range(pos + 8, pos + block_size, 2):
            item = u16(pe, off)
            kind, target = item >> 12, page_rva + (item & 0xFFF)
            if kind not in (0, 10): raise ValueError(f"unsupported x86-64 relocation type {kind}")
            if kind == 10 and (target > image_size or 8 > image_size - target): raise ValueError("relocation target outside image")
        pos += block_size
    return peoff, opt, image_size, sections


def fat12_file(image, short_path):
    bps = u16(image, 11)
    spc = image[13]
    reserved = u16(image, 14)
    fats = image[16]
    root_entries = u16(image, 17)
    fat_sectors = u16(image, 22)
    root_sectors = (root_entries * 32 + bps - 1) // bps
    root_start = (reserved + fats * fat_sectors) * bps
    data_start = (reserved + fats * fat_sectors + root_sectors) * bps
    fat_start = reserved * bps
    fat = image[fat_start:fat_start + fat_sectors * bps]

    def entries(cluster, fixed_root=False):
        if fixed_root:
            start, limit = root_start, root_entries * 32
        else:
            start = data_start + (cluster - 2) * spc * bps
            limit = spc * bps
        for pos in range(start, start + limit, 32):
            row = image[pos:pos + 32]
            if len(row) < 32 or row[0] == 0: break
            if row[0] != 0xE5 and row[11] != 0x0F:
                yield row

    def find(parent, short_name, fixed_root=False):
        for row in entries(parent, fixed_root):
            if row[:11] == short_name:
                return u16(row, 26), u32(row, 28)
        raise AssertionError(f"FAT12 entry not found: {short_name!r}")

    cluster, _ = find(0, short_path[0], True)
    for component in short_path[1:-1]: cluster, _ = find(cluster, component)
    cluster, size = find(cluster, short_path[-1])
    result = bytearray()
    seen = set()
    while cluster < 0xFF8:
        if cluster < 2 or cluster in seen:
            raise AssertionError("invalid or cyclic FAT12 cluster chain")
        seen.add(cluster)
        start = data_start + (cluster - 2) * spc * bps
        result.extend(image[start:start + spc * bps])
        off = cluster + cluster // 2
        word = fat[off] | (fat[off + 1] << 8)
        cluster = (word >> 4) & 0xFFF if cluster & 1 else word & 0xFFF
    return bytes(result[:size])


def fat32_file(disk, partition_lba, short_path):
    bps = u16(disk, partition_lba * 512 + 11)
    part = partition_lba * 512
    spc = disk[part + 13]
    reserved = u16(disk, part + 14)
    fats = disk[part + 16]
    fat_sectors = u32(disk, part + 36)
    root_cluster = u32(disk, part + 44)
    fat_start = part + reserved * bps
    data_start = part + (reserved + fats * fat_sectors) * bps
    fat = disk[fat_start:fat_start + fat_sectors * bps]

    def read_cluster(cluster):
        start = data_start + (cluster - 2) * spc * bps
        return disk[start:start + spc * bps]

    def lookup(cluster, name):
        data = read_cluster(cluster)
        for pos in range(0, len(data), 32):
            row = data[pos:pos + 32]
            if row[0] == 0:
                break
            if row[0] != 0xE5 and row[11] != 0x0F and row[:11] == name:
                return ((u16(row, 20) << 16) | u16(row, 26), u32(row, 28))
        raise AssertionError(f"FAT32 entry not found: {name!r}")

    cluster = root_cluster
    size = 0
    for component in short_path:
        cluster, size = lookup(cluster, component)
    result = bytearray()
    seen = set()
    while cluster < 0x0FFFFFF8:
        if cluster < 2 or cluster in seen:
            raise AssertionError("invalid or cyclic FAT32 cluster chain")
        seen.add(cluster)
        result.extend(read_cluster(cluster))
        cluster = u32(fat, cluster * 4) & 0x0FFFFFFF
    return bytes(result[:size])


class ArtifactTests(unittest.TestCase):
    def test_efi_is_x64_pe32_plus_uefi_application_with_relocations(self):
        pe = EFI.read_bytes()
        peoff, opt, _, sections = parse_pe(pe)
        self.assertEqual(u16(pe, opt + 68), 10)
        coff = peoff + 4
        entry_rva = u32(pe, opt + 16)
        names = []
        entry_in_text = False
        for i in range(u16(pe, coff + 2)):
            sh = coff + 20 + u16(pe, coff + 16) + i * 40
            name = pe[sh:sh + 8].split(b"\0", 1)[0]
            names.append(name)
            virtual_size, rva = u32(pe, sh + 8), u32(pe, sh + 12)
            if name == b".text" and rva <= entry_rva < rva + virtual_size:
                entry_in_text = True
        self.assertTrue(entry_in_text)
        self.assertIn(b".reloc", names)

    def test_pe_relocation_negative_cases(self):
        original = bytearray(EFI.read_bytes())
        _, opt, image_size, _ = parse_pe(original)
        reloc_rva_off = opt + 112 + 5 * 8
        bad_bounds = bytearray(original)
        struct.pack_into("<I", bad_bounds, reloc_rva_off, image_size - 4)
        with self.assertRaises(ValueError): parse_pe(bad_bounds)

        reloc_rva = u32(original, reloc_rva_off)
        reloc_file = None
        coff = u32(original, 0x3C) + 4
        table = coff + 20 + u16(original, coff + 16)
        for i in range(u16(original, coff + 2)):
            sh = table + i * 40
            rva, raw_size, raw_ptr = u32(original, sh + 12), u32(original, sh + 16), u32(original, sh + 20)
            if rva <= reloc_rva < rva + raw_size:
                reloc_file = raw_ptr + reloc_rva - rva
                break
        self.assertIsNotNone(reloc_file)
        unsupported = bytearray(original)
        struct.pack_into("<H", unsupported, reloc_file + 8, 0x3000)  # HIGHLOW is not valid for PE32+.
        with self.assertRaisesRegex(ValueError, "unsupported"):
            parse_pe(unsupported)

        bad_block_size = bytearray(original)
        struct.pack_into("<I", bad_block_size, reloc_file + 4, 0xFFFF)
        with self.assertRaisesRegex(ValueError, "block size"):
            parse_pe(bad_block_size)

        bad_target = bytearray(original)
        struct.pack_into("<I", bad_target, reloc_file, image_size - 0x1000)
        struct.pack_into("<H", bad_target, reloc_file + 8, 0xAFFF)
        with self.assertRaisesRegex(ValueError, "target outside image"):
            parse_pe(bad_target)

        bad_image_base = bytearray(original)
        struct.pack_into("<Q", bad_image_base, opt + 24, 0xFFFFFFFFFFFFFFF0)
        with self.assertRaisesRegex(ValueError, "image base"):
            parse_pe(bad_image_base)

        bad_section = bytearray(original)
        coff = u32(original, 0x3C) + 4
        table = coff + 20 + u16(original, coff + 16)
        struct.pack_into("<I", bad_section, table + 12, image_size - 1)
        with self.assertRaisesRegex(ValueError, "section image bounds"):
            parse_pe(bad_section)

    def test_boot_contract_negative_cases(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".boot-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "boot-validation-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "boot" / "validate.c"), str(ROOT / "tests" / "test_boot_validate.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_interrupt_table_encodings(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".interrupt-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "interrupt-table-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "tests" / "test_interrupt_tables.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_physical_memory_allocator(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".pmm-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "pmm-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "pmm.c"), str(ROOT / "tests" / "test_pmm.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_virtual_memory_core(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".vm-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "vm-core-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "vm_core.c"), str(ROOT / "tests" / "test_vm_core.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_kernel_heap_core(self):
        compiler = shutil.which("gcc") or r"C:\\msys64\\mingw64\\bin\\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".heap-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "heap-core-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "heap_core.c"), str(ROOT / "tests" / "test_heap_core.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_scheduler_queue_core(self):
        compiler = shutil.which("gcc") or r"C:\\msys64\\mingw64\\bin\\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".sched-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "scheduler-core-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "sched_core.c"), str(ROOT / "tests" / "test_sched_core.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_standalone_hello_elf_artifact(self):
        elf = (ROOT / "user" / "hello.elf").read_bytes()
        self.assertGreaterEqual(len(elf), 4096 + 64)
        self.assertEqual(elf[:4], b"\x7fELF")
        self.assertEqual(elf[4:7], bytes((2, 1, 1)))
        self.assertEqual(u16(elf, 16), 2)
        self.assertEqual(u16(elf, 18), 62)
        self.assertEqual(struct.unpack_from("<Q", elf, 24)[0], 0x0000400000000000)
        self.assertEqual((u16(elf, 54), u16(elf, 56)), (56, 1))
        ph = struct.unpack_from("<IIQQQQQQ", elf, 64)
        self.assertEqual(ph[:4], (1, 5, 4096, 0x0000400000000000))
        self.assertEqual(ph[5], ph[6])
        self.assertEqual(ph[7], 4096)
        self.assertLessEqual(ph[2] + ph[5], len(elf))
        self.assertIn(b"Hello from independently assembled hello.elf via syscall write.\r\n", elf)

    def test_elf64_loader_core(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".elf64-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "elf64-loader-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "vm_core.c"),
                            str(ROOT / "kernel" / "elf64_loader.c"),
                            str(ROOT / "tests" / "test_elf64_loader.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe), str(ROOT / "user" / "hello.elf")], check=True, cwd=ROOT)
    def test_syscall_abi_core(self):
        compiler = shutil.which("gcc") or r"C:\\msys64\\mingw64\\bin\\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".syscall-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "syscall-core-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "syscall_core.c"),
                            str(ROOT / "tests" / "test_syscall_core.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_phase9_aosfs_data_vhd_format(self):
        with tempfile.TemporaryDirectory(prefix=".aosfs-image-test-", dir=ROOT / "tests") as td:
            root = Path(td)
            (root / "user").mkdir()
            payload = b"independent ELF fixture"
            (root / "user" / "hello.elf").write_bytes(payload)
            script = root / "make-phase9-data-disk.py"
            shutil.copy2(ROOT / "make-phase9-data-disk.py", script)
            subprocess.run([shutil.which("python") or "python", str(script)], check=True, cwd=root)
            vhd = (root / "asterion-data.vhd").read_bytes()
            self.assertEqual(len(vhd), 16 * 1024 * 1024 + 512)
            footer = bytearray(vhd[-512:])
            self.assertEqual(footer[:8], b"conectix")
            checksum = struct.unpack_from(">I", footer, 64)[0]
            footer[64:68] = bytes(4)
            self.assertEqual(checksum, (~sum(footer)) & 0xFFFFFFFF)
            raw = vhd[:-512]
            self.assertEqual(raw[:8], b"AOSFS1\0\0")
            self.assertEqual(u16(raw, 8), 1)
            self.assertEqual(u32(raw, 16), len(raw) // 512)
            self.assertEqual(raw[512:522], b"hello.elf\0")
            size = u32(raw, 512 + 40)
            self.assertEqual(raw[1024:1024 + size], payload)
            self.assertEqual(u32(raw, 512 + 48), fs_fnv(payload))
    def test_block_device_bounds_and_failures(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".block-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "block-core-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "block_core.c"), str(ROOT / "tests" / "test_block_core.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_read_only_filesystem_validation(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".fs-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "fs-core-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "block_core.c"), str(ROOT / "kernel" / "fs_core.c"),
                            str(ROOT / "tests" / "test_fs_core.c"), "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_lock_order_tracker(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".sync-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "sync-core-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "sync_core.c"), str(ROOT / "tests" / "test_sync_core.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)
    def test_calculator_boundaries_and_malformed_input(self):
        compiler = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
        with tempfile.TemporaryDirectory(prefix=".calc-test-", dir=ROOT / "tests") as td:
            exe = Path(td) / "calc-test.exe"
            subprocess.run([compiler, "-std=c17", "-Wall", "-Wextra", "-Werror",
                            str(ROOT / "kernel" / "calc.c"), str(ROOT / "tests" / "test_calc.c"),
                            "-o", str(exe)], check=True, cwd=ROOT)
            subprocess.run([str(exe)], check=True, cwd=ROOT)

    def test_fat12_image_contains_exact_efi_payload(self):
        image = IMAGE.read_bytes()
        self.assertEqual(len(image), 1_474_560)
        self.assertEqual(image[510:512], b"\x55\xaa")
        extracted = fat12_file(image, [b"EFI        ", b"BOOT       ", b"BOOTX64 EFI"])
        self.assertEqual(extracted, EFI.read_bytes())

    def test_gpt_fat32_vhd_contains_exact_efi_payload(self):
        vhd = UEFI_DISK.read_bytes()
        self.assertEqual(len(vhd), 64 * 1024 * 1024 + 512)
        footer = bytearray(vhd[-512:])
        self.assertEqual(footer[:8], b"conectix")
        footer_uuid = uuid.UUID(bytes=bytes(footer[68:84]))
        self.assertEqual(footer_uuid.version, 5)
        self.assertEqual(footer_uuid, uuid.uuid5(uuid.NAMESPACE_URL, UEFI_DISK.resolve().as_uri()))
        self.assertNotEqual(footer_uuid, uuid.uuid5(uuid.NAMESPACE_DNS, "asterion-os-fixed-vhd-v1"))
        checksum = struct.unpack_from(">I", footer, 64)[0]
        struct.pack_into(">I", footer, 64, 0)
        self.assertEqual(checksum, (~sum(footer)) & 0xFFFFFFFF)
        self.assertEqual(struct.unpack_from(">I", vhd, len(vhd) - 512 + 60)[0], 2)

        disk = vhd[:-512]
        header = bytearray(disk[512:1024])
        self.assertEqual(header[:8], b"EFI PART")
        header_size = u32(header, 12)
        header_crc = u32(header, 16)
        struct.pack_into("<I", header, 16, 0)
        self.assertEqual(header_crc, zlib.crc32(header[:header_size]) & 0xFFFFFFFF)
        entries_lba = struct.unpack_from("<Q", disk, 512 + 72)[0]
        entry_count, entry_size, entries_crc = struct.unpack_from("<III", disk, 512 + 80)
        entries = disk[entries_lba * 512:entries_lba * 512 + entry_count * entry_size]
        self.assertEqual(zlib.crc32(entries) & 0xFFFFFFFF, entries_crc)
        self.assertEqual(entries[:16], uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le)
        partition_lba = struct.unpack_from("<Q", entries, 32)[0]
        partition_end = struct.unpack_from("<Q", entries, 40)[0]
        partition_attributes = struct.unpack_from("<Q", entries, 48)[0]
        self.assertEqual(partition_attributes, 0)
        self.assertGreaterEqual(partition_lba, 34)
        self.assertLess(partition_end, len(disk) // 512 - 33)
        boot = disk[partition_lba * 512:(partition_lba + 1) * 512]
        self.assertEqual(u16(boot, 11), 512)
        self.assertEqual(u32(boot, 32), partition_end - partition_lba + 1)
        self.assertEqual(boot[82:90], b"FAT32   ")
        self.assertEqual(boot[510:512], b"\x55\xaa")
        fat32_cluster_count = u32(boot, 32) - u16(boot, 14) - boot[16] * u32(boot, 36)
        self.assertGreaterEqual(fat32_cluster_count, 65_525)
        self.assertEqual(u32(boot, 44), 2)
        extracted = fat32_file(disk, partition_lba, [b"EFI        ", b"BOOT       ", b"BOOTX64 EFI"])
        self.assertEqual(extracted, EFI.read_bytes())


if __name__ == "__main__":
    unittest.main()

