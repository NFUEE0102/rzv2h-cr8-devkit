#!/usr/bin/env python3
"""讀 R8 黑盒子 @ 0x431F0000(結構版本 5)。

用法:  sudo python3 bb.py [-q]

== 為什麼用 busybox devmem 而不是 mmap ==
Python 的 mmap("/dev/mem") 對這塊位址會拿到**可快取**的映射(核心對判定為
RAM 的實體位址走 normal cacheable),而 R8 那側是 Normal Non-Cacheable +
Shareable —— 屬性不合,一讀行程就當場死掉(0 bytes 輸出、連 SSH 都被帶掉)。

== 結構偏移量 ==
每一版的新欄位都**接在尾端**,舊偏移量原封不動 ——
start-demo.sh 讀的 +0x40(boot_stage)/ +0x44(fault_type)跨版本都有效。
"""
import subprocess, sys

ADDR  = 0x431F0000            # 舊(共用佈局)位址;開機時自動偵測,見下
MAGIC = 0x42423852

# 兩代黑盒子位址(2026-08-29 per-core 遷移):
#   0x439F0000 = per-core 佈局(vring-ctl1-c0 視窗尾端 64KB)
#   0x441F0000 = core1(vring-ctl1-c1 視窗尾端)
#   0x431F0000 = 共用佈局(vring-ctl1 視窗尾端 64KB)
# 自動偵測:magic 有效者優先(新址先試);都無效就用新址報「沒初始化」。
# 也可用 --addr 0xXXXXXXXX 強制指定。
ADDR_CANDIDATES = (0x439F0000, 0x441F0000, 0x431F0000)

F = [("magic",0x00),("version",0x04),("boot_count",0x08),("uptime_ticks",0x0C),
     ("hb_ipi",0x10),("hb_sensor",0x14),("hb_eptcb",0x18),
     ("mhu_isr",0x1C),("sem_give",0x20),("sem_take",0x24),("sem_timeout",0x28),
     ("notif_ok",0x2C),("notif_err",0x30),("send_ok",0x34),("send_err",0x38),
     ("mtx_block",0x3C),("boot_stage",0x40),
     ("fault_type",0x44),("fault_arg0",0x48),("fault_arg1",0x4C),("fault_lr",0x50),
     # fault_task[16] 佔 0x54..0x63
     ("svc_used",0x64),("irq_used",0x68),("sys_used",0x6C),
     ("stack_paint",0x70),("fault_sp_svc",0x74),("fault_sp_irq",0x78),
     ("irq_last",0x7C),("irq_count",0x80),("irq_bad",0x84),("irq_seen",0x88),
     ("irq_culprit",0x8C),("irq_w28",0x90),("irq_w32",0x94),("irq_sp",0x98),
     ("irq_prev",0x9C),
     ("svc_rest",0xA0),("irq_word_addr",0xA4),("irq_word_exp",0xA8),
     ("irq_word_got",0xAC),
     # v5
     ("guard_sp_min",0xB0),("guard_sp_max",0xB4),("rest_min",0xB8),
     ("rest_max",0xBC),("fault_word",0xC0),("fault_word_addr",0xC4),
     # v6
     ("nest_max",0xC8),("guard_skipped",0xCC),
     # v7
     ("irq_off",0xD0),("irq_depth",0xD4),
     # v8
     ("fault_sp_min",0xD8),("fault_sp_max",0xDC),("fault_depth",0xE0),
     ("fault_irq",0xE4),("ring_idx",0xE8),
     # v9  (ring[8][3] 佔 0xEC..0x14B)
     ("yield_defer",0x14C),("yield_apply",0x150),
     # v10
     ("tick_count",0x154),("sns_stage",0x158),
     # v11
     ("epi_sp_before",0x15C),("epi_r2",0x160),("epi_sp_after",0x164),
     # v12
     ("tick_rtos",0x168),
     # v13
     ("epi_entry_addr",0x16C),("epi_entry_val",0x170)]

RW_OFF = 0x174          # ring_word[8][2]

SW = [("sw_exit_cnt",0x1B4),("sw_exit_nest",0x1B8),
      ("sw_svc_cnt",0x1BC),("sw_svc_nest",0x1C0)]

RI_OFF = 0x1C4          # ring_irq[8][3]
EPI_ID = 0x22C          # epi_icciar
BAD = [("bad_intid_cnt",0x230),("bad_intid_first",0x234),("bad_intid_last",0x238)]
MODES = {0x10:"USR",0x11:"FIQ",0x12:"IRQ",0x13:"SVC",0x17:"ABT",0x1B:"UND",0x1F:"SYS"}

RING_OFF = 0xEC          # ring[8][3],每格 3 個字
RING_N   = 8

FAULT = {0:"(無)",1:"堆疊溢位",2:"malloc 失敗",3:"資料中止",
         4:"取指中止",5:"未定義指令",6:"assert"}
CAP = {"svc_used":0x2000, "irq_used":0x4000, "sys_used":0x1000}


def rd(off):
    out = subprocess.check_output(
        ["busybox", "devmem", str(ADDR + off), "32"]).decode().strip()
    return int(out, 16)


# ---- 位址自動偵測(--addr 覆寫)----
if "--addr" in sys.argv:
    ADDR = int(sys.argv[sys.argv.index("--addr") + 1], 0)
else:
    for _cand in ADDR_CANDIDATES:
        ADDR = _cand
        try:
            if rd(0x00) == MAGIC:
                break
        except Exception:
            continue
    else:
        ADDR = ADDR_CANDIDATES[0]


def rd_str(off, n):
    b = b"".join(rd(off + i * 4).to_bytes(4, "little") for i in range(n // 4))
    return b.split(b"\x00")[0].decode("ascii", "replace")


def irqid(x):
    return x & 0x3FF          # ICCIAR bits[9:0] = 中斷編號


v = {n: rd(o) for n, o in F}
if v["magic"] != MAGIC:
    print("magic = 0x%08X —— 黑盒子沒被初始化(R8 沒跑,或還沒到 bb_init)"
          % v["magic"])
    sys.exit(1)

quiet = "-q" in sys.argv
ver = v["version"]

print("boot #%d  version %d  uptime %d ticks  stage %d" %
      (v["boot_count"], ver, v["uptime_ticks"], v["boot_stage"]))

if ver >= 19:
    bc = rd(BAD[0][1]); bf = rd(BAD[1][1]); bl = rd(BAD[2][1])
    if bc:
        print("★★ 向量表索引越界 %d 次(已被邊界檢查擋下)  第一個=%d  最後=%d" % (bc, bf, bl))
    else:
        print("向量表索引越界: 0 次")


ft = v["fault_type"]
if ft:
    print("死因: %s  (type=%d, task=%s)" % (FAULT.get(ft, "?"), ft, rd_str(0x54, 16)))
    print("  arg0=0x%08X  arg1=0x%08X  lr=0x%08X" %
          (v["fault_arg0"], v["fault_arg1"], v["fault_lr"]))
    if ft == 3:
        # interrupt("ABORT") 下 __builtin_return_address(0) 是原始 LR_abt,
        # 資料中止時 = 故障指令 + 8
        print("  故障指令 = 0x%08X  (LR_abt - 8)" % (v["fault_lr"] - 8))
    print("  sp_svc=0x%08X  sp_irq=0x%08X" % (v["fault_sp_svc"], v["fault_sp_irq"]))
    if ver >= 5 and v["fault_word_addr"]:
        w = v["fault_word"]
        verdict = ("字是好的 -> 錯的是 sp_svc 本身"
                   if w in (0, 4) else "★ 字被寫壞了 -> 有人在 guard 檢查後動的手")
        print("  對齊字 [0x%08X] = 0x%08X (%d)   %s" %
              (v["fault_word_addr"], w, w, verdict))
else:
    print("死因: 無")

if ver >= 3:
    print("ISR 毀損偵測:")
    print("  進入 %d 次,最後 IRQ=%d (ICCIAR 0x%X)" %
          (v["irq_count"], irqid(v["irq_last"]), v["irq_last"]))
    if ver >= 4:
        if v["irq_word_addr"]:
            print("  對齊字已校準: addr=0x%08X 應為 %d  (靜止 sp_svc=0x%08X)" %
                  (v["irq_word_addr"], v["irq_word_exp"], v["svc_rest"]))
        else:
            print("  對齊字尚未校準(pump 還沒跑到)")
    if v["irq_seen"]:
        print("  ★★ 抓到了 —— 兇手 IRQ=%d (ICCIAR 0x%X),共 %d 次" %
              (irqid(v["irq_culprit"]), v["irq_culprit"], v["irq_bad"]))
        print("     巢狀深度=%d  前一個 IRQ=%d  當時 sp=0x%08X" %
              (v["irq_depth"], irqid(v["irq_prev"]), v["irq_sp"]))
        print("     對齊字 [sp+%d] = 0x%08X (%d)  —— 只能是 0 或 4" %
              (v["irq_off"], v["irq_word_got"], v["irq_word_got"]))
    else:
        print("  尚未偵測到毀損 (irq_bad=%d)" % v["irq_bad"])

if ver >= 8 and ft:
    print("故障當下(直接取自 SRAM,非 pump 過的陳舊副本):")
    print("  guard sp %08X..%08X   巢狀深度=%d   最後 IRQ=%d" %
          (v["fault_sp_min"], v["fault_sp_max"], v["fault_depth"],
           irqid(v["fault_irq"])))
    idx = v["ring_idx"] & 7
    print("  最後 %d 次中斷(舊 -> 新):" % RING_N)
    for j in range(RING_N):
        k = (idx + j) % RING_N
        ic = rd(RING_OFF + k*12 + 0)
        sp = rd(RING_OFF + k*12 + 4)
        dp = rd(RING_OFF + k*12 + 8)
        mark = "  <- 最後一次" if j == RING_N - 1 else ""
        wa = rd(RW_OFF + k*8 + 0); wv = rd(RW_OFF + k*8 + 4)
        si = rd(RI_OFF + k*12 + 0); sp2 = rd(RI_OFF + k*12 + 4); ra = rd(RI_OFF + k*12 + 8)
        md = MODES.get(sp2 & 0x1F, "?")
        flag = "" if wv in (0,4) else "  <-- ★ 這一層的對齊字已經壞了"
        print("    [%d] IRQ=%-4d sp=0x%08X depth=%d  對齊字[0x%08X]=0x%08X%s%s" %
              (k, irqid(ic), sp, dp, wa, wv, flag, mark))
        print("         sp_irq=0x%08X  被打斷的是 %s(SPSR=0x%08X)  返回=0x%08X" %
              (si, md, sp2, ra))
        continue

SNS = {0:"(未設)", 1:"掏 UART", 2:"羅盤取樣", 3:"pump",
       4:"vTaskDelay", 5:"i2c_wait 中"}

if ver >= 11:   # 不再以 fault 為條件——凍結那種故障沒有 fault
    b, r2, af = v["epi_sp_before"], v["epi_r2"], v["epi_sp_after"]
    print("IRQ 收尾三點(最後一次中斷,由 portASM 直接寫 SRAM):")
    print("  POP {r2} 前 sp = 0x%08X" % b)
    print("  POP 出來的值  = 0x%08X (%d)%s" %
          (r2, r2, "" if r2 in (0,4) else "   <-- ★ 不是 0 或 4,對齊值被破壞"))
    print("  ADD 之後 sp   = 0x%08X%s" %
          (af, "" if af % 4 == 0 else "   <-- ★ 未對齊,下一道 LDM 必炸"))
    if ver >= 13:
        ea, ev = v["epi_entry_addr"], v["epi_entry_val"]
        print("  進入端: addr=0x%08X  值=%d" % (ea, ev))
        ei = rd(EPI_ID); li = v["irq_last"]
        same = irqid(ei) == irqid(li)
        print("  這個收尾屬於 IRQ=%d;最後一筆 guard 是 IRQ=%d%s" %
              (irqid(ei), irqid(li),
               "   (同一個)" if same else "   <-- ★ 不同!收尾與最後那筆 guard 不是同一個中斷"))
        if ea != b:
            print("     ⚠️ 進入端位址與退出端 (0x%08X) 不同 —— 不是同一個框架,比對無效" % b)
        elif ev in (0,4) and r2 not in (0,4):
            print("     ★★ 進入時是 %d(正確),退出時變成 %d —— 中途被改!" % (ev, r2))
        elif ev not in (0,4):
            print("     ★★ 進入時就已經是 %d —— 寫入本身有問題" % ev)
        else:
            print("     (進出一致,這次沒抓到)")
    exp = b + 4 + r2
    if af != exp:
        print("  ⚠️ %08X + 4 + %d = %08X,但實際是 %08X —— sp 在三點之外被改過" % (b, r2, exp, af))

if ver >= 10:
    print("獨立訊號(不經 sensor task):")
    if ver >= 12:
        tr = v["tick_rtos"]; tc = v["tick_count"]
        d = tc - tr
        print("  tick ISR=%d   FreeRTOS tick=%d   落差=%d%s" % (tc, tr, d,
              "" if d < 50 else "   <-- ★ 排程器沒跟上,xTaskIncrementTick() 沒跑完"))
    print("  tick_count = %d   sensor task 停在: %s (%d)" %
          (v["tick_count"], SNS.get(v["sns_stage"], "?"), v["sns_stage"]))

if ver >= 9:
    print("巢狀 yield 攔截(修正):")
    print("  攔下 %d 次  補做 %d 次%s" %
          (v["yield_defer"], v["yield_apply"],
           "   <- 這些原本每一次都可能讓 R8 死掉" if v["yield_defer"] else ""))

if ver >= 6:
    n = v["nest_max"]
    print("中斷巢狀:")
    print("  最大深度 %d%s   guard 因巢狀跳過 %d 次" %
          (n, "   ★ 超過陣列 32 格!" if n > 32 else "", v["guard_skipped"]))

if ver >= 5:
    gmin, gmax = v["guard_sp_min"], v["guard_sp_max"]
    rmin, rmax = v["rest_min"], v["rest_max"]
    print("漂移偵測:")
    print("  guard sp   0x%08X .. 0x%08X   幅度 %d bytes%s" %
          (gmin, gmax, gmax - gmin, "   ★ sp_svc 會漂!" if gmax != gmin else "   (穩定)"))
    print("  靜止 sp_svc 0x%08X .. 0x%08X  幅度 %d bytes%s" %
          (rmin, rmax, rmax - rmin, "   ★ 會漂!" if rmax != rmin else "   (穩定)"))

if ver >= 2:
    print("堆疊水位 (上色 bitmask=0x%X):" % v["stack_paint"])
    for n in ("svc_used", "irq_used", "sys_used"):
        cap = CAP[n]
        print("  %-4s %6d / %5d bytes  (%5.1f%%)%s" %
              (n[:3].upper(), v[n], cap, 100.0 * v[n] / cap,
               "   <-- 爆了" if v[n] >= cap - 64 else ""))

if not quiet:
    print("計數器:")
    skip = set(n for n, _ in F) - set(
        ["hb_ipi","hb_sensor","hb_eptcb","mhu_isr","sem_give","sem_take",
         "sem_timeout","notif_ok","notif_err","send_ok","send_err","mtx_block"])
    for n, _ in F:
        if n not in skip:
            print("  %-12s %10d" % (n, v[n]))
