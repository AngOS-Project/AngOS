import math
import struct
import sys
from pathlib import Path

S = 512
START = 2048
SECTORS = 131072
RESERVED = 32
FATS = 2
FATSECTORS = 1024
DATA = RESERVED + FATS * FATSECTORS
CLUSTERS = SECTORS - DATA
EOC = 0x0FFFFFFF

def p16(b, o, v):
    struct.pack_into("<H", b, o, v)

def p32(b, o, v):
    struct.pack_into("<I", b, o, v)

def ent(name, ext, attr, cluster=0, size=0):
    b = bytearray(32)
    b[:8] = name.upper().encode("ascii").ljust(8, b" ")[:8]
    b[8:11] = ext.upper().encode("ascii").ljust(3, b" ")[:3]
    b[11] = attr
    p16(b, 20, cluster >> 16)
    p16(b, 26, cluster & 0xffff)
    p32(b, 28, size)
    return bytes(b)

def directory(entries):
    b = b"".join(entries) + bytes(32)
    if len(b) > S:
        raise ValueError("Directory exceeds one cluster")
    return b.ljust(S, b"\0")

def main():
    if len(sys.argv) != 4:
        raise SystemExit("Usage: python tools/make_fat32_test_disk.py OUTPUT EFI KERNEL")

    out, efi, kernel = map(Path, sys.argv[1:])
    if not efi.is_file() or not kernel.is_file():
        raise SystemExit("Build AngOS.efi and AngOS.elf first")

    efi_data = efi.read_bytes()
    kernel_data = kernel.read_bytes()
    test_data = (b"AngOS FAT32 runtime test file.\r\n" * 32)[:700]
    print("FAT32 multi-cluster test file: 700 bytes")
    nested_data = b"AngOS nested directory test.\r\n"

    if CLUSTERS < 65525 or CLUSTERS + 2 > FATSECTORS * (S // 4):
        raise SystemExit("Invalid FAT32 geometry")

    fat_entries = {0: 0x0FFFFFF8, 1: 0xFFFFFFFF, 2: EOC, 3: EOC, 4: EOC, 5: EOC}
    clusters = {}
    nxt = 6

    def allocate(data):
        nonlocal nxt
        if not data:
            return 0
        count = math.ceil(len(data) / S)
        ids = list(range(nxt, nxt + count))
        if ids[-1] >= CLUSTERS + 2:
            raise ValueError("Test data exceeds FAT32 volume")
        for i, cluster in enumerate(ids):
            fat_entries[cluster] = ids[i + 1] if i + 1 < len(ids) else EOC
            clusters[cluster] = data[i * S:(i + 1) * S].ljust(S, b"\0")
        nxt += count
        return ids[0]

    test_cluster = allocate(test_data)
    nested_cluster = allocate(nested_data)
    efi_cluster = allocate(efi_data)
    kernel_cluster = allocate(kernel_data)

    clusters[2] = directory([
        ent("EFI", "", 0x10, 3),
        ent("TESTDIR", "", 0x10, 5),
        ent("RUNTIME", "TXT", 0x20, test_cluster, len(test_data)),
        ent("EMPTY", "TXT", 0x20, 0, 0),
        ent("ANGOS", "ELF", 0x20, kernel_cluster, len(kernel_data))
    ])
    clusters[3] = directory([
        ent(".", "", 0x10, 3),
        ent("..", "", 0x10, 2),
        ent("BOOT", "", 0x10, 4)
    ])
    clusters[4] = directory([
        ent(".", "", 0x10, 4),
        ent("..", "", 0x10, 3),
        ent("BOOTX64", "EFI", 0x20, efi_cluster, len(efi_data))
    ])
    clusters[5] = directory([
        ent(".", "", 0x10, 5),
        ent("..", "", 0x10, 2),
        ent("NESTED", "TXT", 0x20, nested_cluster, len(nested_data))
    ])

    mbr = bytearray(S)
    e = 446
    mbr[e] = 0x80
    mbr[e + 4] = 0xEF
    p32(mbr, e + 8, START)
    p32(mbr, e + 12, SECTORS)
    mbr[510:512] = b"\x55\xaa"

    boot = bytearray(S)
    boot[:3] = b"\xeb\x58\x90"
    boot[3:11] = b"ANGOS   "
    p16(boot, 11, S)
    boot[13] = 1
    p16(boot, 14, RESERVED)
    boot[16] = FATS
    boot[21] = 0xF8
    p16(boot, 24, 63)
    p16(boot, 26, 255)
    p32(boot, 28, START)
    p32(boot, 32, SECTORS)
    p32(boot, 36, FATSECTORS)
    p32(boot, 44, 2)
    p16(boot, 48, 1)
    p16(boot, 50, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    p32(boot, 67, 0xA2601028)
    boot[71:82] = b"ANGOS FAT32"
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xaa"

    fsinfo = bytearray(S)
    p32(fsinfo, 0, 0x41615252)
    p32(fsinfo, 484, 0x61417272)
    p32(fsinfo, 488, CLUSTERS - (nxt - 2))
    p32(fsinfo, 492, nxt)
    p32(fsinfo, 508, 0xAA550000)

    fat = bytearray(FATSECTORS * S)
    for cluster, value in fat_entries.items():
        p32(fat, cluster * 4, value)

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.truncate((START + SECTORS) * S)

        def write(lba, data):
            f.seek(lba * S)
            f.write(data)

        write(0, mbr)
        write(START, boot)
        write(START + 1, fsinfo)
        write(START + 6, boot)
        write(START + 7, fsinfo)
        write(START + RESERVED, fat)
        write(START + RESERVED + FATSECTORS, fat)

        for cluster, data in clusters.items():
            write(START + DATA + cluster - 2, data)

    print(f"Created FAT32 test disk: {out}")
    print(f"Size: {(START + SECTORS) * S} bytes; data clusters: {CLUSTERS}; file clusters: {nxt - 6}")

if __name__ == "__main__":
    main()
