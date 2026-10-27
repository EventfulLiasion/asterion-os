"""Build a fixed VHD with a GPT EFI System Partition containing Asterion."""
from pathlib import Path
import math
import struct
import uuid
import zlib

ROOT = Path(__file__).resolve().parent
EFI = ROOT / "EFI" / "BOOT" / "BOOTX64.EFI"
DISK = ROOT / "asterion-uefi.vhd"

SECTOR = 512
DISK_SECTORS = 131_072  # 64 MiB fixed VHD
PARTITION_START = 2_048
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
RESERVED_SECTORS = 32
SECTORS_PER_CLUSTER = 1
FAT_COUNT = 2

payload = EFI.read_bytes()
partition_sectors = DISK_SECTORS - 34 - PARTITION_START
fat_sectors = math.ceil((partition_sectors - RESERVED_SECTORS + 2) * 4 / (SECTOR * SECTORS_PER_CLUSTER + 8))
cluster_count = partition_sectors - RESERVED_SECTORS - FAT_COUNT * fat_sectors
if cluster_count < 65_525:
    raise SystemExit("EFI partition is too small for a valid FAT32 volume.")

disk = bytearray(DISK_SECTORS * SECTOR)

# Protective MBR for GPT-aware firmware and tools.
mbr = memoryview(disk)[:SECTOR]
mbr[446 + 4] = 0xEE
struct.pack_into("<II", mbr, 446 + 8, 1, DISK_SECTORS - 1)
mbr[510:512] = b"\x55\xaa"

disk_guid = uuid.uuid5(uuid.NAMESPACE_DNS, "asterion-os-uefi-disk-v1")
partition_guid = uuid.uuid5(uuid.NAMESPACE_DNS, "asterion-os-uefi-esp-v1")
partition_end = PARTITION_START + partition_sectors - 1
entry = bytearray(GPT_ENTRY_SIZE)
entry[0:16] = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
entry[16:32] = partition_guid.bytes_le
struct.pack_into("<QQQ", entry, 32, PARTITION_START, partition_end, 0)
entry[56:128] = "EFI System Partition".encode("utf-16le").ljust(72, b"\0")
partition_table = bytearray(GPT_ENTRIES * GPT_ENTRY_SIZE)
partition_table[:GPT_ENTRY_SIZE] = entry
partition_table_crc = zlib.crc32(partition_table) & 0xFFFFFFFF

def write_gpt_header(lba: int, backup_lba: int, entries_lba: int) -> None:
    header = bytearray(SECTOR)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        b"EFI PART",
        0x00010000,
        92,
        0,
        0,
        lba,
        backup_lba,
        34,
        DISK_SECTORS - 34,
        disk_guid.bytes_le,
        entries_lba,
        GPT_ENTRIES,
        GPT_ENTRY_SIZE,
        partition_table_crc,
    )
    struct.pack_into("<I", header, 16, zlib.crc32(header[:92]) & 0xFFFFFFFF)
    offset = lba * SECTOR
    disk[offset : offset + SECTOR] = header

disk[2 * SECTOR : 2 * SECTOR + len(partition_table)] = partition_table
backup_entries_lba = DISK_SECTORS - 1 - len(partition_table) // SECTOR
disk[backup_entries_lba * SECTOR : (DISK_SECTORS - 1) * SECTOR] = partition_table
write_gpt_header(1, DISK_SECTORS - 1, 2)
write_gpt_header(DISK_SECTORS - 1, 1, backup_entries_lba)

partition_offset = PARTITION_START * SECTOR
fat_start_sector = RESERVED_SECTORS
data_start_sector = RESERVED_SECTORS + FAT_COUNT * fat_sectors
data_start = partition_offset + data_start_sector * SECTOR

boot = bytearray(SECTOR)
boot[0:3] = b"\xeb\x58\x90"
boot[3:11] = b"ASTERION"
struct.pack_into("<H", boot, 11, SECTOR)
boot[13] = SECTORS_PER_CLUSTER
struct.pack_into("<H", boot, 14, RESERVED_SECTORS)
boot[16] = FAT_COUNT
struct.pack_into("<H", boot, 17, 0)
struct.pack_into("<H", boot, 19, 0)
boot[21] = 0xF8
struct.pack_into("<H", boot, 22, 0)
struct.pack_into("<HHI", boot, 24, 63, 16, PARTITION_START)
struct.pack_into("<I", boot, 32, partition_sectors)
struct.pack_into("<I", boot, 36, fat_sectors)
struct.pack_into("<HHI", boot, 40, 0, 0, 2)
struct.pack_into("<HH", boot, 48, 1, 6)
boot[64] = 0x80
boot[66] = 0x29
struct.pack_into("<I", boot, 67, 0x41535452)
boot[71:82] = b"ASTERIONOS "
boot[82:90] = b"FAT32   "
boot[510:512] = b"\x55\xaa"
disk[partition_offset : partition_offset + SECTOR] = boot

fsinfo = bytearray(SECTOR)
struct.pack_into("<I", fsinfo, 0, 0x41615252)
struct.pack_into("<I", fsinfo, 484, 0x61417272)
file_clusters = max(1, math.ceil(len(payload) / (SECTOR * SECTORS_PER_CLUSTER)))
used_clusters = 3 + file_clusters  # root, EFI, BOOT, and payload chain
struct.pack_into("<II", fsinfo, 488, cluster_count - used_clusters, 5 + file_clusters)
fsinfo[510:512] = b"\x55\xaa"
disk[partition_offset + SECTOR : partition_offset + 2 * SECTOR] = fsinfo
disk[partition_offset + 6 * SECTOR : partition_offset + 7 * SECTOR] = boot
disk[partition_offset + 7 * SECTOR : partition_offset + 8 * SECTOR] = fsinfo

fat = bytearray(fat_sectors * SECTOR)
struct.pack_into("<III", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF)

def fat_set(cluster: int, value: int) -> None:
    struct.pack_into("<I", fat, cluster * 4, value & 0x0FFFFFFF)

def chain_clusters(first: int, count: int) -> list[int]:
    clusters = list(range(first, first + count))
    for index, cluster in enumerate(clusters):
        fat_set(cluster, clusters[index + 1] if index + 1 < len(clusters) else 0x0FFFFFFF)
    return clusters

fat_set(3, 0x0FFFFFFF)
fat_set(4, 0x0FFFFFFF)
payload_chain = chain_clusters(5, file_clusters)
for copy in range(FAT_COUNT):
    start = partition_offset + (fat_start_sector + copy * fat_sectors) * SECTOR
    disk[start : start + len(fat)] = fat

def cluster_offset(cluster: int) -> int:
    return data_start + (cluster - 2) * SECTOR * SECTORS_PER_CLUSTER

def dir_entry(name: bytes, attributes: int, cluster: int, size: int = 0) -> bytes:
    row = bytearray(32)
    row[:11] = name.ljust(11, b" ")[:11]
    row[11] = attributes
    struct.pack_into("<H", row, 20, cluster >> 16)
    struct.pack_into("<H", row, 26, cluster & 0xFFFF)
    struct.pack_into("<I", row, 28, size)
    return bytes(row)

def write_directory(cluster: int, rows: list[bytes]) -> None:
    data = bytearray(SECTOR * SECTORS_PER_CLUSTER)
    for index, row in enumerate(rows):
        data[index * 32 : (index + 1) * 32] = row
    offset = cluster_offset(cluster)
    disk[offset : offset + len(data)] = data

write_directory(2, [dir_entry(b"EFI", 0x10, 3)])
write_directory(3, [dir_entry(b".", 0x10, 3), dir_entry(b"..", 0x10, 2), dir_entry(b"BOOT", 0x10, 4)])
write_directory(4, [
    dir_entry(b".", 0x10, 4),
    dir_entry(b"..", 0x10, 3),
    dir_entry(b"BOOTX64 EFI", 0x20, payload_chain[0], len(payload)),
])
for index, cluster in enumerate(payload_chain):
    chunk = payload[index * SECTOR : (index + 1) * SECTOR]
    offset = cluster_offset(cluster)
    disk[offset : offset + len(chunk)] = chunk

# A fixed VHD consists of the raw disk followed by a 512-byte footer.
footer = bytearray(SECTOR)
footer[0:8] = b"conectix"
struct.pack_into(">IIQ", footer, 8, 2, 0x00010000, 0xFFFFFFFFFFFFFFFF)
struct.pack_into(">4sI4sQQ", footer, 28, b"AOS ", 0x00010000, b"Wi2k", len(disk), len(disk))
sectors = DISK_SECTORS
cylinders = min(sectors // (16 * 63), 65535)
struct.pack_into(">HBB", footer, 56, cylinders, 16, 63)
struct.pack_into(">I", footer, 60, 2)
# Keep a stable UUID for this output path so VirtualBox registrations survive
# rebuilds, while separate build directories receive distinct VHD identities.
footer_uuid = uuid.uuid5(uuid.NAMESPACE_URL, DISK.resolve().as_uri())
footer[68:84] = footer_uuid.bytes
struct.pack_into(">I", footer, 64, (~sum(footer)) & 0xFFFFFFFF)

DISK.write_bytes(disk + footer)
print(
    f"Wrote {DISK} ({DISK.stat().st_size} bytes): GPT ESP FAT32, "
    f"EFI/BOOT/BOOTX64.EFI ({len(payload)} bytes)."
)
