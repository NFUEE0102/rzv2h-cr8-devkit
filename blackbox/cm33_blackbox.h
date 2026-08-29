/* CM33 黑盒子 —— R8 黑盒子(r8_blackbox.h v21)思想的 Cortex-M33 精簡版。
 *
 * 位址:phys 0x431F0000 = CM33 視角 0x831F0000(DDR secure alias,
 * 與本韌體 rsctbl @0x82F00000 同域同法)。這塊在 CM33 法定區
 * vring-ctl1@43100000 UIO 視窗尾端 64KB —— 正是 per-core 遷移前
 * core0 黑盒子的舊家,A55 的 bb.py 本來就會讀這個位址。
 *
 * 佈局鐵律:magic/version/boot_count/uptime(+0x00..0x0F)、
 * boot_stage(+0x40)、fault_type(+0x44)、tick_count(+0x154)
 * 與 R8 版同 offset —— start 腳本的 +0x44 開機健檢與 bb.py 的
 * 摘要邏輯兩核通用。magic 不同("BB33")讓工具辨識核型。
 */
#ifndef CM33_BLACKBOX_H
#define CM33_BLACKBOX_H

#include <stdint.h>

#define CM33_BB_ADDR   (0x831F0000u)   /* CM33 view of phys 0x431F0000 */
#define CM33_BB_MAGIC  (0x33334242u)   /* "BB33" little-endian */
#define CM33_BB_VER    (1u)

typedef struct
{
    volatile uint32_t magic;         /* 0x00 */
    volatile uint32_t version;       /* 0x04 */
    volatile uint32_t boot_count;    /* 0x08 */
    volatile uint32_t uptime_ticks;  /* 0x0C(pump 寫,FreeRTOS tick)*/
    uint32_t          _pad0[12];     /* 0x10..0x3F */
    volatile uint32_t boot_stage;    /* 0x40:1=bb_init 12=rpmsg vdev ready */
    volatile uint32_t fault_type;    /* 0x44:0=無 3=HardFault */
    volatile uint32_t cfsr;          /* 0x48 SCB->CFSR */
    volatile uint32_t hfsr;          /* 0x4C SCB->HFSR */
    volatile uint32_t bfar;          /* 0x50 */
    volatile uint32_t mmfar;         /* 0x54 */
    volatile uint32_t stacked_pc;    /* 0x58 例外堆疊框的 PC */
    volatile uint32_t stacked_lr;    /* 0x5C */
    volatile uint32_t exc_return;    /* 0x60 */
    uint32_t          _pad1[(0x154 - 0x64) / 4];
    volatile uint32_t tick_count;    /* 0x154(與 R8 版同 offset)*/
} cm33_bb_t;

#define CM33_BB  ((cm33_bb_t *) CM33_BB_ADDR)

void cm33_bb_init(void);
void cm33_bb_pump(void);

#define CM33_BB_STAGE(n)  do { CM33_BB->boot_stage = (n); } while (0)

#endif
