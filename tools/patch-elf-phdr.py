#!/usr/bin/env python3
"""把 CR8 elf 裡「不需要載入」的 DDR 段從 PT_LOAD 改成 PT_NULL。

== 為什麼必須做 ==
連結器會替 .rtos_heap_section / .resource_table / .noinit / .mhu_shmem /
.vring 這些 (NOLOAD) 區段各產生一個 PT_LOAD,雖然 FileSiz 都是 0。
Linux remoteproc 的 rproc_elf_load_segments() 看到 PT_LOAD 就要求該位址
落在它認得的 carveout 裡,而這些位址是我們自己在 DDR 手動管理的,於是:

    remoteproc remoteproc1: bad phdr da 0x42f00000 mem 0x190
    remoteproc remoteproc1: Failed to load program segments: -22
    remoteproc remoteproc1: Boot failed: -22

改成 PT_NULL 之後載入器直接跳過,section 表原封不動(位址、大小、
檔案版面全都不變),只動 phdr 的 type 欄位 —— 這是最小的改動。

== 判斷條件 ==
p_type == PT_LOAD 且 p_filesz == 0 且 p_paddr >= 0x40000000
⚠️ 位址下限不能拿掉:DTCM 的 .dtcm_noload_section(0x20000,memsz 0x12f24)
   filesz 也是 0,但它在可運作的 elf 裡**仍然是 PT_LOAD**,動了會出事。

用法: patch-elf-phdr.py <elf>   (就地修改,先備份 .prepatch)
"""
import io, os, shutil, struct, sys

PT_NULL, PT_LOAD = 0, 1
DDR_MIN = 0x40000000

def main(path):
    with io.open(path, "rb") as f:
        b = bytearray(f.read())

    if b[:4] != b"\x7fELF":
        print("  X 不是 ELF"); return 1
    if b[4] != 1 or b[5] != 1:
        print("  X 只支援 ELF32 little-endian"); return 1

    e_phoff, = struct.unpack_from("<I", b, 0x1C)
    e_phentsize, = struct.unpack_from("<H", b, 0x2A)
    e_phnum, = struct.unpack_from("<H", b, 0x2C)
    if e_phentsize != 32:
        print("  X phentsize=%d,預期 32" % e_phentsize); return 1

    changed = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = \
            struct.unpack_from("<6I", b, o)
        if p_type == PT_LOAD and p_filesz == 0 and p_paddr >= DDR_MIN:
            struct.pack_into("<I", b, o, PT_NULL)
            changed.append((i, p_paddr, p_memsz))

    if not changed:
        print("  已是修正過的狀態(沒有要改的段)"); return 0

    shutil.copy2(path, path + ".prepatch")
    with io.open(path, "wb") as f:
        f.write(b)

    # ---- 自我驗證:重讀檔案確認真的寫進去了 ----
    with io.open(path, "rb") as f:
        c = bytearray(f.read())
    bad = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, _, _, p_paddr, p_filesz, _ = struct.unpack_from("<6I", c, o)
        if p_type == PT_LOAD and p_filesz == 0 and p_paddr >= DDR_MIN:
            bad.append(i)
    if bad:
        print("  X 寫入後驗證失敗,phdr %s 仍是 PT_LOAD" % bad); return 1

    nload = sum(1 for i in range(e_phnum)
                if struct.unpack_from("<I", c, e_phoff + i * e_phentsize)[0] == PT_LOAD)
    for i, pa, mz in changed:
        print("  phdr[%2d]  0x%08X  memsz 0x%06X  LOAD -> NULL" % (i, pa, mz))
    print("  OK  共 %d 個;PT_LOAD 剩 %d / %d" % (len(changed), nload, e_phnum))
    return 0

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("用法: %s <elf>" % sys.argv[0]); sys.exit(1)
    sys.exit(main(sys.argv[1]))
