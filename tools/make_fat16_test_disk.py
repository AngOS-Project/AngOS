import struct
import sys
from pathlib import Path

S = 512
START = 2048
TOTAL = 32768
RESERVED = 1
FAT_SECTORS = 128
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // S
DATA_START = RESERVED + 2 * FAT_SECTORS + ROOT_SECTORS
CLUSTERS = TOTAL - DATA_START
EOC = 0xFFFF

def p16(b, o, v):
    struct.pack_into("<H", b, o, v)

def p32(b, o, v):
    struct.pack_into("<I", b, o, v)

def entry(name, ext, attr, cluster=0, size=0):
    b = bytearray(32)
    b[:8] = name.encode("ascii").ljust(8, b" ")
    b[8:11] = ext.encode("ascii").ljust(3, b" ")
    b[11] = attr
    p16(b, 26, cluster)
    p32(b, 28, size)
    return bytes(b)

def main():
    if CLUSTERS < 4085 or CLUSTERS > 65524:
        raise SystemExit("Generated volume is not FAT16 geometry.")

    runtime = (b"AngOS FAT16 multi-cluster test.\\r\\n" * 32)[:700]
    nested = b"AngOS FAT16 nested file test.\\r\\n"

    chains = {
        2: EOC,
        3: 4,
        4: EOC,
        5: EOC
    }

    fat = bytearray(FAT_SECTORS * S)
    p16(fat, 0, 0xFFF8)
    p16(fat, 2, 0xFFFF)

    for cluster, value in chains.items():
        p16(fat, cluster * 2, value)

    mbr = bytearray(S)
    mbr[446] = 0
    mbr[450] = 0x06
    p32(mbr, 454, START)
    p32(mbr, 458, TOTAL)
    mbr[510:512] = b"\\x55\\xaa"

    boot = bytearray(S)
    boot[:3] = b"\\xeb\\x3c\\x90"
    boot[3:11] = b"ANGOS   "
    p16(boot, 11, S)
    boot[13] = 1
    p16(boot, 14, RESERVED)
    boot[16] = 2
    p16(boot, 17, ROOT_ENTRIES)
    p16(boot, 19, TOTAL)
    boot[21] = 0xF8
    p16(boot, 22, FAT_SECTORS)
    p16(boot, 24, 63)
    p16(boot, 26, 255)
    p32(boot, 28, START)
    boot[36] = 0x80
    boot[38] = 0x29
    p32(boot, 39, 0xA2601032)
    boot[43:54] = b"ANGOS TEST "
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\\x55\\xaa"

    root = b"".join([
        entry("SUBDIR", "", 0x10, 2),
        entry("RUNTIME", "TXT", 0x20, 3, len(runtime)),
        entry("EMPTY", "TXT", 0x20, 0, 0),
        bytes(32)
    ]).ljust(ROOT_SECTORS * S, b"\\0")

    subdir = b"".join([
        entry(".", "", 0x10, 2),
        entry("..", "", 0x10, 0),
        entry("NESTED", "TXT", 0x20, 5, len(nested)),
        bytes(32)
    ]).ljust(S, b"\\0")

    out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/fat16-test-disk.img")
    out.parent.mkdir(parents=True, exist_ok=True)

    with out.open("wb") as f:
        f.truncate((START + TOTAL) * S)

        def write(lba, data):
            f.seek(lba * S)
            f.write(data)

        write(0, mbr)
        write(START, boot)
        write(START + RESERVED, fat)
        write(START + RESERVED + FAT_SECTORS, fat)

        root_lba = START + RESERVED + 2 * FAT_SECTORS
        write(root_lba, root)

        data_lba = START + DATA_START
        write(data_lba, subdir)
        write(data_lba + 1, runtime[:S].ljust(S, b"\\0"))
        write(data_lba + 2, runtime[S:].ljust(S, b"\\0"))
        write(data_lba + 3, nested.ljust(S, b"\\0"))

    print("FAT16 test disk created.")
    print("FAT16 geometry:", CLUSTERS, "clusters")
    print("Multi-cluster file:", len(runtime), "bytes")
    print("Empty file: EMPTY.TXT")

if __name__ == "__main__":
    main()
