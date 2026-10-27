"""Create a minimal FAT12 UEFI removable-media image for Asterion OS."""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parent
EFI = ROOT / "EFI" / "BOOT" / "BOOTX64.EFI"
IMAGE = ROOT / "asterion-uefi.img"

SECTORS = 2880
SECTOR_SIZE = 512
FAT_SECTORS = 9
ROOT_SECTORS = 14
DATA_START = 1 + 2 * FAT_SECTORS + ROOT_SECTORS
image = bytearray(SECTORS * SECTOR_SIZE)

# FAT12 floppy boot record. UEFI loads BOOTX64.EFI through the FAT filesystem.
b = memoryview(image)[:SECTOR_SIZE]
b[0:3] = b"\xeb\x3c\x90"
b[3:11] = b"ASTERION"
struct.pack_into("<H", b, 11, SECTOR_SIZE)
b[13] = 1
struct.pack_into("<H", b, 14, 1)
b[16] = 2
struct.pack_into("<H", b, 17, 224)
struct.pack_into("<H", b, 19, SECTORS)
b[21] = 0xF0
struct.pack_into("<H", b, 22, FAT_SECTORS)
struct.pack_into("<H", b, 24, 18)
struct.pack_into("<H", b, 26, 2)
b[36] = 0
b[38] = 0x29
struct.pack_into("<I", b, 39, 0x41535452)
b[43:54] = b"ASTERIONOS "
b[54:62] = b"FAT12   "
b[510:512] = b"\x55\xaa"

def fat_set(fat: bytearray, cluster: int, value: int) -> None:
    off = cluster + cluster // 2
    value &= 0xFFF
    if cluster & 1:
        fat[off] = (fat[off] & 0x0F) | ((value << 4) & 0xF0)
        fat[off + 1] = (value >> 4) & 0xFF
    else:
        fat[off] = value & 0xFF
        fat[off + 1] = (fat[off + 1] & 0xF0) | ((value >> 8) & 0x0F)

fat = bytearray(FAT_SECTORS * SECTOR_SIZE)
fat[:3] = b"\xf0\xff\xff"
for cluster in (2, 3):
    fat_set(fat, cluster, 0xFFF)
for copy in range(2):
    start = (1 + copy * FAT_SECTORS) * SECTOR_SIZE
    image[start : start + len(fat)] = fat

root = (1 + 2 * FAT_SECTORS) * SECTOR_SIZE

def entry(offset: int, name: bytes, attr: int, cluster: int, size: int = 0) -> None:
    image[offset : offset + 11] = name.ljust(11, b" ")[:11]
    image[offset + 11] = attr
    struct.pack_into("<H", image, offset + 26, cluster)
    struct.pack_into("<I", image, offset + 28, size)

entry(root, b"EFI", 0x10, 2)
def cluster_offset(cluster: int) -> int:
    return (DATA_START + cluster - 2) * SECTOR_SIZE

efi_dir = cluster_offset(2)
entry(efi_dir, b".", 0x10, 2)
entry(efi_dir + 32, b"..", 0x10, 0)
entry(efi_dir + 64, b"BOOT", 0x10, 3)
boot_dir = cluster_offset(3)
entry(boot_dir, b".", 0x10, 3)
entry(boot_dir + 32, b"..", 0x10, 2)
payload = EFI.read_bytes()
clusters = max(1, (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE)
chain = list(range(4, 4 + clusters))
if cluster_offset(chain[-1]) + SECTOR_SIZE > len(image):
    raise SystemExit("EFI application exceeds the FAT12 floppy image capacity.")
for i, cluster in enumerate(chain):
    fat_set(fat, cluster, chain[i + 1] if i + 1 < len(chain) else 0xFFF)
for copy in range(2):
    start = (1 + copy * FAT_SECTORS) * SECTOR_SIZE
    image[start : start + len(fat)] = fat
entry(boot_dir + 64, b"BOOTX64 EFI", 0x20, 4, len(payload))
for i, cluster in enumerate(chain):
    part = payload[i * SECTOR_SIZE : (i + 1) * SECTOR_SIZE]
    image[cluster_offset(cluster) : cluster_offset(cluster) + len(part)] = part
IMAGE.write_bytes(image)
print(f"Wrote {IMAGE} ({len(image)} bytes), EFI payload {len(payload)} bytes")
