/*
 * R8 黑盒子的實作 + FreeRTOS fault hook(2026-08-24)
 *
 * 設計參考 Renesas 官方的 rzv2h_drone_px4:它的 vApplicationMallocFailedHook
 * 與 vApplicationStackOverflowHook 都是「先解除馬達,再關中斷,再停住」。
 * 那個順序不是隨便寫的 —— GPT 是硬體驅動的,R8 死了 PWM 還會用最後的
 * 佔空比一直輸出(我們自己的 doc 15 §6.1 也實測過這件事)。
 * 所以任何 fault 的第一件事都是把 PWM 拉到安全狀態。
 */

#include <string.h>

#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"

#include "r8_blackbox.h"

/* ------------------------------------------------------------------ */
/* 安全:把所有 PWM 輸出停掉                                            */
/* ------------------------------------------------------------------ */

/* pwm_test.c 定義的 GPT 控制區塊表。用 weak 參照 ——
 * 就算之後把 PWM 拿掉,這個檔還是能編。 */
extern void pwm_emergency_stop(void) __attribute__((weak));

static void r8_disarm_outputs(void)
{
    /* ⚠️ 順序要緊:先解除輸出,再關中斷。
     * 關了中斷就什麼都做不了了,而 GPT 不會因為 R8 停住就停止輸出。 */
    if (NULL != pwm_emergency_stop)
    {
        pwm_emergency_stop();
    }
}

/* ------------------------------------------------------------------ */
/* 黑盒子                                                              */
/* ------------------------------------------------------------------ */

/* open-amp 診斷打卡的落點(rpmsg_virtio.c 以 weak 呼叫)*/
void r8_blackbox_stage_probe(unsigned int n);
void r8_blackbox_stage_probe(unsigned int n)
{
    R8_BB_STAGE(n);
}

void r8_blackbox_init(void)
{
    uint32_t boots = 0u;

    /* 保留 boot_count:重開機時 DDR 內容不一定會清,所以能看出這是第幾次啟動。
     * magic 不對就從 0 開始。 */
    if (R8_BB_MAGIC == R8_BB->magic)
    {
        boots = R8_BB->boot_count;
    }

    memset((void *) R8_BB, 0, sizeof(r8_blackbox_t));
    R8_BB->version    = R8_BB_VERSION;
    R8_BB->boot_count = boots + 1u;
    R8_BB->fault_type = R8_FAULT_NONE;

    /* magic 最後才寫 —— 在它出現之前,所有 R8_BB_INC() 都是 no-op,
     * 所以不會有「初始化到一半就被計數器踩到」的問題。 */
    R8_BB->magic = R8_BB_MAGIC;

    /* magic 就緒後才上色 —— R8_BB_SET 在那之前是 no-op */
    r8_stack_paint();
}

void r8_blackbox_fault(uint32_t type, uint32_t a0, uint32_t a1,
                       uint32_t lr, const char * task)
{
    if (R8_BB_MAGIC != R8_BB->magic)
    {
        /* 黑盒子還沒初始化就出事 —— 至少留下 magic 與型別 */
        memset((void *) R8_BB, 0, sizeof(r8_blackbox_t));
        R8_BB->magic   = R8_BB_MAGIC;
        R8_BB->version = R8_BB_VERSION;
    }

    /* 只記第一次。後續的 fault 多半是第一次的連鎖反應,
     * 覆蓋掉會讓真正的死因消失。 */
    if (R8_FAULT_NONE != R8_BB->fault_type)
    {
        return;
    }

    R8_BB->fault_arg0 = a0;
    R8_BB->fault_arg1 = a1;
    R8_BB->fault_lr   = lr;

    if (NULL != task)
    {
        uint32_t i;
        for (i = 0u; (i < (sizeof(R8_BB->fault_task) - 1u)) && ('\0' != task[i]); i++)
        {
            R8_BB->fault_task[i] = task[i];
        }
        R8_BB->fault_task[i] = '\0';
    }

    /* type 最後寫 —— 讀取端看到非 0 就代表其餘欄位已經齊了 */
    R8_BB->fault_type = type;
}

/* ------------------------------------------------------------------ */
/* FreeRTOS hooks                                                      */
/* ------------------------------------------------------------------ */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char * pcTaskName);
void vApplicationStackOverflowHook (TaskHandle_t xTask, char * pcTaskName)
{
    /* 剩餘堆疊只在 FreeRTOS 有編進這個 API 時才拿得到。
     * 這份組態沒開 INCLUDE_uxTaskGetStackHighWaterMark,所以條件編譯包起來 ——
     * 拿不到就報 0,不值得為了它多動一個組態旗標。 */
#if (defined(INCLUDE_uxTaskGetStackHighWaterMark) && (INCLUDE_uxTaskGetStackHighWaterMark == 1))
    UBaseType_t left = (NULL != xTask) ? uxTaskGetStackHighWaterMark(xTask) : 0u;
#else
    UBaseType_t left = 0u;
#endif

    r8_blackbox_fault(R8_FAULT_STACK_OVERFLOW,
                      (uint32_t) xTask,
                      (uint32_t) (left * sizeof(StackType_t)),
                      (uint32_t) __builtin_return_address(0),
                      pcTaskName);

    r8_disarm_outputs();          /* 先解除馬達,再關中斷(官方 PX4 的順序) */
    taskDISABLE_INTERRUPTS();
    for ( ; ; )
    {
        /* 停在這裡。黑盒子已經寫好,從 Linux devmem 讀 0x431F0000 */
    }
}

void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook (void)
{
    r8_blackbox_fault(R8_FAULT_MALLOC_FAILED,
                      (uint32_t) xPortGetFreeHeapSize(),
                      (uint32_t) xPortGetMinimumEverFreeHeapSize(),
                      (uint32_t) __builtin_return_address(0),
                      "malloc");

    r8_disarm_outputs();
    taskDISABLE_INTERRUPTS();
    for ( ; ; )
    {
    }
}

/* ------------------------------------------------------------------ */
/* metal mutex 爭用計數                                                 */
/* ------------------------------------------------------------------ */
/* 由 libmetal 的 freertos/mutex.h 遞增(短自旋耗盡、真的要 block 時)。
 * 定義放這裡而不是讓 libmetal include 黑盒子的標頭,是為了不讓底層
 * 反向相依到應用層。停擺時從 Linux 讀 mtx_block 就知道是不是卡在鎖上。 */
volatile unsigned int g_metal_mutex_blocks = 0u;

/* 堆疊掃描的節流基準。pump 會用到,所以定義必須在 pump 之前 ——
 * 這個檔案已經踩過三次順序問題,新的全域一律放這裡。 */
static uint32_t g_r8_scan_last = 0u;

/* 定義在下面的「ISR 毀損偵測」區段。pump 在它之前,所以先宣告 —— 
 * 計數器放 DTCM 而不是黑盒子,是因為每次中斷都要動,
 * 寫 uncached DDR 會把 230400 baud 的 UART 中斷拖慢而改變時序。 */
extern volatile uint32_t g_r8_irq_last;
extern volatile uint32_t g_r8_irq_count;
extern volatile uint32_t g_r8_epi[8];
extern volatile uint32_t g_r8_rw[8][2];
extern volatile uint32_t g_r8_sw[4];
extern volatile uint32_t g_r8_ri[8][3];
volatile uint32_t g_r8_ri[8][3];   /* sp_irq / 保存的 SPSR / 返回位址 */
/* [5]=收尾開頭的 CPSR  [6]=收尾時中斷未遮蔽的次數 */
volatile uint32_t g_r8_rw[8][2];   /* guard 檢查的 (位址, 值) */
/* [0]=switch_before_exit 次數 [1]=其最大巢狀層數
 * [2]=SVC_Handler 次數        [3]=其最大巢狀層數 */
volatile uint32_t g_r8_sw[4];
volatile uint32_t g_r8_ri[8][3];   /* sp_irq / 保存的 SPSR / 返回位址 */
/* [5]=收尾開頭的 CPSR  [6]=收尾時中斷未遮蔽的次數 */
extern volatile uint32_t g_r8_sp_min;
extern volatile uint32_t g_r8_sp_max;
extern volatile uint32_t g_r8_word_addr;
extern volatile uint32_t g_r8_word_exp;
extern volatile uint32_t g_r8_nest_max;
extern volatile uint32_t g_r8_nest_skip;
extern volatile uint32_t g_r8_word_off;
extern volatile uint32_t g_r8_ring[8][3];
extern volatile uint32_t g_r8_ring_idx;
extern volatile uint32_t g_r8_yield_defer;
extern volatile uint32_t g_r8_yield_apply;
void r8_irq_guard_calibrate(void);

/* 由 sensor task 週期性搬進黑盒子(mutex.h 那層看不到黑盒子的結構) */
void r8_blackbox_pump(void)
{
    R8_BB_SET(mtx_block, g_metal_mutex_blocks);
    uint32_t now = (uint32_t) xTaskGetTickCount();

    R8_BB_SET(uptime_ticks, now);

    /* ⚠️ 節流到約 1 Hz。這個 pump 是在 sensor task 的主迴圈裡呼叫的,
     * 而那個迴圈跑 ~859 Hz(不是我當初註解寫的 5 Hz),
     * 每次 r8_stack_scan() 要掃近 7000 個字 —— 白費太多。 */
    if ((now - g_r8_scan_last) >= configTICK_RATE_HZ)
    {
        g_r8_scan_last = now;
        r8_stack_scan();
    }
    R8_BB_SET(irq_last,  g_r8_irq_last);
    R8_BB_SET(irq_count, g_r8_irq_count);
    /* 收尾三點也搬一份。故障時 abort handler 會再直接從 SRAM 覆寫一次 ——
     * 這裡搬只是為了讓儀器在正常運作時就能被驗證。 */
    R8_BB_SET(epi_sp_before, g_r8_epi[0]);
    R8_BB_SET(epi_r2,        g_r8_epi[1]);
    R8_BB_SET(epi_sp_after,  g_r8_epi[2]);
    R8_BB_SET(epi_entry_addr, g_r8_epi[3]);
    R8_BB_SET(epi_entry_val,  g_r8_epi[4]);
    R8_BB_SET(sw_exit_cnt,  g_r8_sw[0]);
    R8_BB_SET(sw_exit_nest, g_r8_sw[1]);
    R8_BB_SET(sw_svc_cnt,   g_r8_sw[2]);
    R8_BB_SET(sw_svc_nest,  g_r8_sw[3]);
    R8_BB_SET(epi_cpsr,     g_r8_epi[5]);
    R8_BB_SET(epi_unmasked, g_r8_epi[6]);
    R8_BB_SET(epi_icciar,   g_r8_epi[7]);
    r8_irq_guard_calibrate();
}

/* ------------------------------------------------------------------ */
/* 模式堆疊水位探針                                                     */
/* ------------------------------------------------------------------ */
/*
 * 為什麼要做這個:0x7C0 的 `pop {r0-r4,ip}` 從界外位址 LDM 而 abort,
 * 代表 0x7A8 的 `pop {r2}` 讀回的不是 0x784 存進去的對齊修正量(只可能
 * 是 0 或 4)。兩次故障 sp 都正好是 __SvcStackLimit + 0x5E —— 固定 94
 * bytes,與堆疊大小無關。要分辨是「SVC 堆疊向下爆掉」還是「某個 ISR
 * 寫壞了那個字」,只能實際量水位。
 */

#define R8_PAINT   (0xA5A5A5A5u)
#define R8_MODE_SVC  (0xD3u)     /* I=1 F=1 T=0 mode=10011 */
#define R8_MODE_IRQ  (0xD2u)
#define R8_MODE_SYS  (0xDFu)

/* 連結器符號:Base = 低位址,Limit = 高位址(堆疊由 Limit 往下長) */
extern uint32_t __SvcStackBase[], __SvcStackLimit[];
extern uint32_t __IrqStackBase[], __IrqStackLimit[];
extern uint32_t __SysStackBase[], __SysStackLimit[];

/* 讀別的處理器模式的 sp。整段 asm 不碰堆疊,所以中途切模式是安全的;
 * 期間 I/F 都遮蔽,免得在 IRQ 模式裡真的收到 IRQ 而踩爛 lr_irq。 */
uint32_t r8_read_mode_sp (uint32_t mode_byte)
{
    uint32_t sp_val = 0u, saved = 0u;

    __asm__ volatile (
        "mrs   %1, cpsr\n\t"
        "cpsid if\n\t"
        "msr   cpsr_c, %2\n\t"
        "mov   %0, sp\n\t"
        "msr   cpsr_c, %1\n\t"
        : "=&r" (sp_val), "=&r" (saved)
        : "r" (mode_byte)
        : "memory");

    return sp_val;
}

static uint32_t r8_paint_one (uint32_t * base, uint32_t * limit, uint32_t mode_byte)
{
    uint32_t   sp  = r8_read_mode_sp(mode_byte);
    uint32_t * top = limit;
    uint32_t * p;

    /* 如果現在正站在這個堆疊上(開機時多半如此),只塗到 sp 下方 64 bytes,
     * 否則會把自己的框架抹掉。代價是水位基準線多算 64 bytes,無所謂。 */
    if ((sp > (uint32_t) (uintptr_t) base) && (sp <= (uint32_t) (uintptr_t) limit))
    {
        top = (uint32_t *) (uintptr_t) ((sp - 64u) & ~3u);
        if (top <= base)
        {
            return 0u;
        }
    }

    for (p = base; p < top; p++)
    {
        *p = R8_PAINT;
    }

    return 1u;
}

void r8_stack_paint (void)
{
    uint32_t ok = 0u;

    ok |= r8_paint_one(__SvcStackBase, __SvcStackLimit, R8_MODE_SVC) ? 1u : 0u;
    ok |= r8_paint_one(__IrqStackBase, __IrqStackLimit, R8_MODE_IRQ) ? 2u : 0u;
    ok |= r8_paint_one(__SysStackBase, __SysStackLimit, R8_MODE_SYS) ? 4u : 0u;

    R8_BB_SET(stack_paint, ok);
}

static uint32_t r8_used_one (const uint32_t * base, const uint32_t * limit)
{
    const uint32_t * p = base;

    while ((p < limit) && (R8_PAINT == *p))
    {
        p++;
    }

    return (uint32_t) ((uintptr_t) limit - (uintptr_t) p);
}

void r8_stack_scan (void)
{
    R8_BB_SET(svc_used, r8_used_one(__SvcStackBase, __SvcStackLimit));
    R8_BB_SET(irq_used, r8_used_one(__IrqStackBase, __IrqStackLimit));
    R8_BB_SET(sys_used, r8_used_one(__SysStackBase, __SysStackLimit));
}

/* ------------------------------------------------------------------ */
/* ISR 毀損偵測                                                        */
/* ------------------------------------------------------------------ */
/*
 * 為什麼是這個位址:portASM 的 IRQ_Handler 在 SVC 模式下
 *     push {r0-r4, r12}   -> sp = L-24
 *     and  r2, r2, #4     -> r2 只可能是 0 或 4
 *     sub  sp, sp, r2
 *     push {r2}           -> r2=0 時存在 L-28;r2=4 時存在 L-32
 * 離開時 `pop {r2}` / `add sp, sp, r2` 把它加回去。若讀回鬼值,sp 就飛到
 * 界外,下一道 `pop {r0-r4, ip}` 是 LDM(要求字對齊)當場 Data Abort。
 *
 * 那個字**每次中斷進入都會被重寫**,所以任務期間寫壞它是無害的 ——
 * 兇手必定在 push/pop 之間動手,也就是這個 ISR 自己。
 */

/* 每次中斷都要動的計數器。放在 SRAM(0x0818xxxx)而不是黑盒子所在的 DDR ——
 * 黑盒子那塊是 Normal Non-Cacheable,每個中斷寫一次會把 230400 baud 的
 * UART 中斷拖慢而改變時序。由 pump 每 5 Hz 搬進黑盒子即可。 */
volatile uint32_t g_r8_irq_last  = 0u;
volatile uint32_t g_r8_irq_prev  = 0u;
volatile uint32_t g_r8_irq_count = 0u;

/* 校準結果。addr 為 0 代表還沒校準,此時 guard 直接返回(不產生偽陽性)。 */
volatile uint32_t g_r8_word_addr = 0u;
volatile uint32_t g_r8_word_exp  = 0u;
volatile uint32_t g_r8_sp_min    = 0u;
volatile uint32_t g_r8_sp_max    = 0u;
volatile uint32_t g_r8_nest_max  = 0u;
volatile uint32_t g_r8_nest_skip = 0u;
volatile uint32_t g_r8_nest_events = 0u;   /* depth>=2 的累計 */
volatile uint32_t g_r8_word_off  = 0u;   /* 對齊字相對 guard sp 的偏移 */

/* 最後 8 次中斷的現場。放 SRAM,每次中斷寫 3 個字(單週期),
 * 故障時再由 abort handler 一次搬進黑盒子。 */
volatile uint32_t g_r8_ring[8][3];
volatile uint32_t g_r8_ring_idx = 0u;

/* IRQ 收尾三點快照,由 portASM.asm 每次中斷直接寫。放 SRAM。
 * [0]=POP {r2} 之前的 sp  [1]=POP 出來的值  [2]=ADD 之後的 sp  [3]=保留 */
volatile uint32_t g_r8_epi[8];
volatile uint32_t g_r8_rw[8][2];   /* guard 檢查的 (位址, 值) */
/* [0]=switch_before_exit 次數 [1]=其最大巢狀層數
 * [2]=SVC_Handler 次數        [3]=其最大巢狀層數 */
volatile uint32_t g_r8_sw[4];
volatile uint32_t g_r8_ri[8][3];   /* sp_irq / 保存的 SPSR / 返回位址 */
/* [5]=收尾開頭的 CPSR  [6]=收尾時中斷未遮蔽的次數 */
volatile uint32_t g_r8_yield_defer = 0u;   /* 攔下的次數 */
volatile uint32_t g_r8_yield_apply = 0u;   /* 補做的次數 */
volatile uint32_t g_r8_yield_pend  = 0u;   /* 有一筆待補 */

/* FSP 的巢狀深度計數器(bsp_irq.c)。呼叫 guard 時它已經遞減回來,
 * 所以「非巢狀」= 0,而不是 1。 */
extern uint8_t g_current_interrupt_pointer;

/* 由 bsp_common_interrupt_handler 在遞增後呼叫 */
/* 由 bsp_common_interrupt_handler 在偵測到越界索引時呼叫。
 * 這是 2026-08-27 追出來的根因:向量表索引沒有邊界檢查。 */
void r8_bad_intid (uint32_t intid)
{
    R8_BB_INC(bad_intid_cnt);
    if (R8_BB_MAGIC == R8_BB->magic)
    {
        if (0u == R8_BB->bad_intid_first) { R8_BB->bad_intid_first = intid; }
        R8_BB->bad_intid_last = intid;
    }
}

void r8_nest_probe (uint32_t depth)
{
    if (depth > g_r8_nest_max) { g_r8_nest_max = depth; }
    if (depth >= 2u) { g_r8_nest_events++; }
}

/* 由 pump 呼叫。跑在 sensor task(SYS 模式),任務執行時不可能有 ISR 在跑,
 * 所以此刻的 sp_svc 就是靜止值。 */
void r8_irq_guard_calibrate (void)
{
    uint32_t rest;
    uint32_t align;

    rest = r8_read_mode_sp(R8_MODE_SVC);
    if ((rest <= (uint32_t) (uintptr_t) __SvcStackBase) ||
        (rest >  (uint32_t) (uintptr_t) __SvcStackLimit))
    {
        return;                       /* 不在 SVC 堆疊範圍內,不敢用 */
    }

    /* portASM: push {r0-r4, ip} -> -24;and r2, sp, #4;sub sp, sp, r2;
     *          push {r2} -> 對齊字 */
    /* 每次都記靜止值的範圍 —— 任務側看到的漂移 */
    if ((0u == R8_BB->rest_min) || (rest < R8_BB->rest_min)) { R8_BB_SET(rest_min, rest); }
    if (rest > R8_BB->rest_max)                              { R8_BB_SET(rest_max, rest); }
    R8_BB_SET(guard_sp_min, g_r8_sp_min);
    R8_BB_SET(guard_sp_max, g_r8_sp_max);
    R8_BB_SET(nest_max, g_r8_nest_max);
    R8_BB_SET(guard_skipped, g_r8_nest_skip);
    R8_BB_SET(nest_events, g_r8_nest_events);
    R8_BB_SET(yield_defer, g_r8_yield_defer);
    R8_BB_SET(yield_apply, g_r8_yield_apply);

    if (0u != g_r8_word_addr)
    {
        return;                       /* 位址只校準一次,但上面的範圍每次都更新 */
    }

    /* 2026-08-27 起框架是 PUSH {r0-r4, r12, lr} = 28 bytes(原 24)。 */
    align = (rest - 28u) & 4u;

    R8_BB_SET(svc_rest, rest);
    R8_BB_SET(irq_word_exp, align);
    R8_BB_SET(irq_word_addr, rest - 28u - align - 4u);

    g_r8_word_exp  = align;
    g_r8_word_addr = rest - 28u - align - 4u;   /* 最後才設,設了就開始檢查 */
}

void r8_irq_guard (uint32_t icciar)
{
    uint32_t addr = g_r8_word_addr;
    uint32_t got;
    uint32_t sp = 0u;

    g_r8_irq_prev = g_r8_irq_last;
    g_r8_irq_last = icciar;
    g_r8_irq_count++;

    /* 每次都記 sp 的範圍。sp 與該次中斷的 sp_svc 差一個固定的框架大小,
     * 所以 min != max 就代表 sp_svc 會漂 —— 這是 (a)/(b) 的第一個分辨器,
     * 而且漂移往上時堆疊水位完全看不出來(上色只塗到靜止 sp 下方)。 */
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    if ((0u == g_r8_sp_min) || (sp < g_r8_sp_min)) { g_r8_sp_min = sp; }
    if (sp > g_r8_sp_max)                          { g_r8_sp_max = sp; }

    {
        uint32_t i = g_r8_ring_idx & 7u;
        g_r8_ring[i][0] = icciar;
        g_r8_ring[i][1] = sp;
        g_r8_ring[i][2] = (uint32_t) g_current_interrupt_pointer;
        /* 同一個索引,記下這一層框架的對齊字位址與當下的值。
         * g_r8_word_off 校準完成前 addr 會是 sp,值不具意義,但無妨。 */
        {
            uint32_t wa = sp + g_r8_word_off;
            g_r8_rw[i][0] = wa;
            g_r8_rw[i][1] = (0u != g_r8_word_off)
                          ? *(const volatile uint32_t *) (uintptr_t) wa : 0u;
        }
        {
            /* IRQ 模式的返回狀態。r8_read_mode_sp 只切模式讀 sp、不碰堆疊,
             * 期間 I/F 都遮蔽,安全。 */
            uint32_t si = r8_read_mode_sp(R8_MODE_IRQ);
            g_r8_ri[i][0] = si;
            /* sp_irq 指向 SPSR,+4 是返回位址。si 必須落在 IRQ 堆疊內才敢讀。 */
            if ((si >= (uint32_t) (uintptr_t) __IrqStackBase) &&
                (si <  (uint32_t) (uintptr_t) __IrqStackLimit))
            {
                g_r8_ri[i][1] = *(const volatile uint32_t *) (uintptr_t) si;
                g_r8_ri[i][2] = *(const volatile uint32_t *) (uintptr_t) (si + 4u);
            }
            else
            {
                g_r8_ri[i][1] = 0u;
                g_r8_ri[i][2] = 0u;
            }
        }
        g_r8_ring_idx   = i + 1u;
    }

    if (0u == addr)
    {
        return;                       /* 還沒校準 */
    }

    /* 對齊字的**絕對**位址隨巢狀深度變動,但相對 guard 自己的 sp 是固定的。
     * 先在非巢狀時把偏移量量出來,之後任何深度都能檢查。
     * (v6 巢狀時直接跳過,正好漏掉唯一會出事的那些。) */
    if (0u == g_r8_word_off)
    {
        if (0u != g_current_interrupt_pointer)
        {
            g_r8_nest_skip++;
            return;                   /* 還沒校準,巢狀時先不看 */
        }
        g_r8_word_off = addr - sp;
    }

    /* 值只能是 0 或 4(portASM 的 `and r2, r2, #4`),與深度無關 */
    got = *(const volatile uint32_t *) (uintptr_t) (sp + g_r8_word_off);
    if ((0u == got) || (4u == got))
    {
        return;                       /* 正常路徑:一次讀 + 兩次比較 */
    }

    R8_BB_INC(irq_bad);
    if ((R8_BB_MAGIC == R8_BB->magic) && (0u == R8_BB->irq_seen))
    {
        R8_BB_SET(irq_culprit, icciar);
        R8_BB_SET(irq_word_got, got);
        R8_BB_SET(irq_sp,  sp);
        R8_BB_SET(irq_prev, g_r8_irq_prev);
        R8_BB_SET(irq_depth, (uint32_t) g_current_interrupt_pointer);
        R8_BB_SET(irq_off, g_r8_word_off);
        R8_BB_SET(irq_seen, 1u);
    }
}

/* ------------------------------------------------------------------ */
/* CPU 例外處理器                                                       */
/* ------------------------------------------------------------------ */
/*
 * startup.asm 的預設處理器是 `b <self>` —— 掉進去就永遠出不來,而且
 * abort/undef mode 預設遮蔽 IRQ,所以連 MHU 中斷都停。表現出來就是
 * 「整顆 R8 突然死掉,所有 task 同時凍結,而且沒有任何 FreeRTOS hook 觸發」。
 *
 * 那些符號是 .weak 的,在這裡用 C 覆寫就會蓋掉。先把死因寫進黑盒子、
 * 解除馬達,再停住 —— 這樣至少從 Linux 讀得到「死在哪個位址」。
 *
 * CP15 暫存器(ARMv7-R):
 *   DFSR = c5,c0,0   資料中止的原因
 *   DFAR = c6,c0,0   資料中止的目標位址  <- 最有價值的一個
 *   IFSR = c5,c0,1   指令中止的原因
 *   IFAR = c6,c0,2   指令中止的位址
 */

__attribute__((interrupt("ABORT"))) void Abort_Handler(void);
void Abort_Handler (void)
{
    uint32_t dfsr = 0u, dfar = 0u;

    __asm__ volatile ("MRC p15, 0, %0, c5, c0, 0" : "=r" (dfsr));
    __asm__ volatile ("MRC p15, 0, %0, c6, c0, 0" : "=r" (dfar));

    /* 先抓 sp,再記 fault —— r8_blackbox_fault 只記第一次 */
    /* ★ 分辨器:故障當下那個對齊字到底是什麼。
     *   讀到 122 之類 -> 字在 guard 檢查後被寫壞(可能性 a)
     *   讀到 0 或 4   -> 字是好的,錯的是 sp_svc 本身(可能性 b) */
    /* 這幾個直接從 SRAM 讀,不用 pump 過的陳舊副本 —— 那是 v7 的盲點 */
    /* ⚠️ 這些平常由 pump 從 SRAM 搬進黑盒子,但 pump 跑在 sensor task 裡 ——
     * 故障當下它若卡住(實測卡在 i2c_wait),欄位就停在舊值。
     * 2026-08-27 踩過:nest_max 顯示 1,但環形緩衝記著 depth=1 的巢狀。
     * 所以這裡一律直接從 SRAM 覆寫。 */
    R8_BB_SET(nest_max,      g_r8_nest_max);
    R8_BB_SET(guard_sp_min,  g_r8_sp_min);
    R8_BB_SET(guard_sp_max,  g_r8_sp_max);
    R8_BB_SET(guard_skipped, g_r8_nest_skip);
    R8_BB_SET(nest_events, g_r8_nest_events);
    R8_BB_SET(yield_defer,   g_r8_yield_defer);
    R8_BB_SET(epi_sp_before, g_r8_epi[0]);
    R8_BB_SET(epi_r2,        g_r8_epi[1]);
    R8_BB_SET(epi_sp_after,  g_r8_epi[2]);
    R8_BB_SET(epi_entry_addr, g_r8_epi[3]);
    R8_BB_SET(epi_entry_val,  g_r8_epi[4]);
    R8_BB_SET(sw_exit_cnt,  g_r8_sw[0]);
    R8_BB_SET(sw_exit_nest, g_r8_sw[1]);
    R8_BB_SET(sw_svc_cnt,   g_r8_sw[2]);
    R8_BB_SET(sw_svc_nest,  g_r8_sw[3]);
    R8_BB_SET(epi_cpsr,     g_r8_epi[5]);
    R8_BB_SET(epi_unmasked, g_r8_epi[6]);
    R8_BB_SET(epi_icciar,   g_r8_epi[7]);

    R8_BB_SET(fault_sp_min, g_r8_sp_min);
    R8_BB_SET(fault_sp_max, g_r8_sp_max);
    R8_BB_SET(fault_depth,  (uint32_t) g_current_interrupt_pointer);
    R8_BB_SET(fault_irq,    g_r8_irq_last);
    R8_BB_SET(ring_idx,     g_r8_ring_idx);
    if (R8_BB_MAGIC == R8_BB->magic)
    {
        uint32_t k;
        for (k = 0u; k < R8_RING_N; k++)
        {
            R8_BB->ring[k][0] = g_r8_ring[k][0];
            R8_BB->ring[k][1] = g_r8_ring[k][1];
            R8_BB->ring[k][2] = g_r8_ring[k][2];
            R8_BB->ring_word[k][0] = g_r8_rw[k][0];
            R8_BB->ring_word[k][1] = g_r8_rw[k][1];
            R8_BB->ring_irq[k][0] = g_r8_ri[k][0];
            R8_BB->ring_irq[k][1] = g_r8_ri[k][1];
            R8_BB->ring_irq[k][2] = g_r8_ri[k][2];
        }
    }

    if (0u != g_r8_word_addr)
    {
        R8_BB_SET(fault_word_addr, g_r8_word_addr);
        R8_BB_SET(fault_word,
                  *(const volatile uint32_t *) (uintptr_t) g_r8_word_addr);
    }
    R8_BB_SET(fault_sp_svc, r8_read_mode_sp(R8_MODE_SVC));
    R8_BB_SET(fault_sp_irq, r8_read_mode_sp(R8_MODE_IRQ));
    r8_stack_scan();

    r8_blackbox_fault(R8_FAULT_DATA_ABORT, dfar, dfsr,
                      (uint32_t) __builtin_return_address(0), "data_abort");
    r8_disarm_outputs();
    for ( ; ; )
    {
    }
}

__attribute__((interrupt("ABORT"))) void Prefetch_Handler(void);
void Prefetch_Handler (void)
{
    uint32_t ifsr = 0u, ifar = 0u;

    __asm__ volatile ("MRC p15, 0, %0, c5, c0, 1" : "=r" (ifsr));
    __asm__ volatile ("MRC p15, 0, %0, c6, c0, 2" : "=r" (ifar));

    r8_blackbox_fault(R8_FAULT_PREFETCH_ABORT, ifar, ifsr,
                      (uint32_t) __builtin_return_address(0), "prefetch");
    r8_disarm_outputs();
    for ( ; ; )
    {
    }
}

__attribute__((interrupt("UNDEF"))) void Undefined_Handler(void);
void Undefined_Handler (void)
{
    r8_blackbox_fault(R8_FAULT_UNDEF_INSTR, 0u, 0u,
                      (uint32_t) __builtin_return_address(0), "undef");
    r8_disarm_outputs();
    for ( ; ; )
    {
    }
}
