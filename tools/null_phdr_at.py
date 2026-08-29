#!/usr/bin/env python3
"""把指定 vaddr 的 PT_LOAD 轉 PT_NULL(ELF32 little-endian)。

用途(2026-08-29,雙核並跑):FSP 的 .wait_data_other 段(4 bytes)落在
「另一核」的 SRAM 尾 —— remoteproc 載入它等於把對方核的初始化仲裁字清零,
而且位址不在本核的 rproc 窗裡會直接 bad phdr -22。這段在單核世界無害,
雙核並跑必須 NULL 掉:
  core1 韌體:python3 null_phdr_at.py <elf> 0x081BFFFC   (core0 的 wait 字)
  core0 韌體:python3 null_phdr_at.py <elf> 0x081FFFFC   (core1 的 wait 字)
"""
import struct
import sys

path = sys.argv[1]
targets = {int(x, 0) for x in sys.argv[2:]}
data = bytearray(open(path, "rb").read())
assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1, "不是 ELF32 LE"
phoff = struct.unpack_from("<I", data, 28)[0]
phentsize = struct.unpack_from("<H", data, 42)[0]
phnum = struct.unpack_from("<H", data, 44)[0]
hit = 0
for i in range(phnum):
    off = phoff + i * phentsize
    ptype, poff, vaddr, paddr, filesz, memsz = struct.unpack_from("<IIIIII", data, off)
    if ptype == 1 and vaddr in targets:
        struct.pack_into("<I", data, off, 0)
        print(f"  phdr[{i}] vaddr 0x{vaddr:08X} filesz 0x{filesz:X}  LOAD -> NULL")
        hit += 1
open(path, "wb").write(data)
if not hit:
    print("  (沒有命中任何段)")
    sys.exit(1)
