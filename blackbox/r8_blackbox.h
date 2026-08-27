/*
 * R8 黑盒子(2026-08-24)
 *
 * rpmsg 停擺時,R8 那側完全沒有可觀測性 —— 沒有 console、rpmsg 本身就是壞掉的
 * 那條路。連續三次靠推理下手(barrier domain、FreeRTOS heap、metal mutex 自旋)
 * 都沒解決,因為每一次都只能事後從「停住了」反推。
 *
 * 這個結構讓 R8 把關鍵計數器寫進一塊 **Linux 用 devmem 直接讀得到**的記憶體,
 * 停擺當下就能分辨:
 *   - 哪個執行緒還在動(心跳)
 *   - MHU 中斷有沒有進來、semaphore 有沒有被取走
 *   - IpiTask 有沒有處理 vring、endpoint callback 有沒有被叫到
 *   - rpmsg_send 有沒有送出去
 *   - metal mutex 有沒有在爭用
 *   - 有沒有 fault(堆疊溢位 / malloc 失敗 / data abort)
 *
 * == 位置選擇 ==
 * 0x431F0000 = vring-ctl1 UIO 視窗(0x43100000 + 1 MB)的尾端 64 KB。
 *   - Linux 已有 UIO 映射,`devmem` 直接讀,不必改裝置樹
 *   - 在 R8 的 MPU region 9 內(Normal Non-Cacheable + Shareable),
 *     寫下去立刻可見,不必做 cache 維護
 *   - 該視窗實際只用到前約 27 KB(兩個 vring),這裡離得夠遠
 */
#ifndef R8_BLACKBOX_H
#define R8_BLACKBOX_H

#include <stdint.h>

#define R8_BB_ADDR     (0x431F0000u)
#define R8_BB_MAGIC    (0x42423852u)
#define R8_RING_N      (8u)   /* 最後 N 次中斷的現場 */   /* 'R','8','B','B' little-endian */
#define R8_BB_VERSION  (21u)

/* fault_type */
#define R8_FAULT_NONE            (0u)
#define R8_FAULT_STACK_OVERFLOW  (1u)
#define R8_FAULT_MALLOC_FAILED   (2u)
#define R8_FAULT_DATA_ABORT      (3u)
#define R8_FAULT_PREFETCH_ABORT  (4u)
#define R8_FAULT_UNDEF_INSTR     (5u)
#define R8_FAULT_ASSERT          (6u)

typedef struct
{
    uint32_t magic;          /* R8_BB_MAGIC —— 不是這個值就代表黑盒子沒被初始化 */
    uint32_t version;
    uint32_t boot_count;     /* 每次韌體啟動 +1。確認讀到的不是上一輪的殘留 */
    uint32_t uptime_ticks;   /* 由 tick hook 更新;停住就代表連 tick 都不動了 */

    /* ---- 心跳:誰還活著 ---------------------------------------------- */
    uint32_t hb_ipi;         /* IpiTaskId 迴圈次數(優先權 4) */
    uint32_t hb_sensor;      /* sensor_task 迴圈次數(優先權 2) */
    uint32_t hb_eptcb;       /* rpmsg endpoint callback 進入次數 */

    /* ---- MHU / semaphore -------------------------------------------- */
    uint32_t mhu_isr;        /* MHU ISR 進入次數 */
    uint32_t sem_give;       /* xSemaphoreGiveFromISR 次數 */
    uint32_t sem_take;       /* IpiTask 成功取得 semaphore 次數 */
    uint32_t sem_timeout;    /* Take 逾時次數 —— 有值就代表通知被吞過 */

    /* ---- vring / rpmsg ---------------------------------------------- */
    uint32_t notif_ok;       /* remoteproc_get_notification() 回 0 */
    uint32_t notif_err;      /* 回非 0 */
    uint32_t send_ok;        /* rpmsg_send() >= 0 */
    uint32_t send_err;       /* rpmsg_send() < 0 */

    /* ---- metal mutex 爭用 -------------------------------------------- */
    uint32_t mtx_block;      /* 短自旋耗盡、真的 block 的次數 */
    /* 原本規劃是 mtx_spin_max,改用來記開機階段 —— async abort 的 LR 不可信,
     * 只能靠逐點打卡才知道死在哪一段。欄位位置不動,讀取器不必改。 */
    uint32_t boot_stage;     /* 1 bb_init 2 ioport 3 main_task 4 init_system
                              * 5 platform_init 6 pre_vdev 7 vdev_ok 8 app
                              * 10 blinky 11 sensors_start 12 pwm_entry */

    /* ---- 死因 --------------------------------------------------------- */
    uint32_t fault_type;     /* R8_FAULT_* */
    uint32_t fault_arg0;     /* data abort: DFAR;stack overflow: task handle */
    uint32_t fault_arg1;     /* data abort: DFSR;stack overflow: 剩餘 bytes */
    uint32_t fault_lr;       /* 出事時的 LR —— 反查是誰呼叫的 */
    char     fault_task[16]; /* 出事的 task 名稱 */

    /* ---- 堆疊水位(2026-08-25,version 2 起)--------------------------- */
    /* 開機時把三個模式堆疊塗成 0xA5A5A5A5,之後週期性從底部往上掃第一個
     * 不是塗料的字 —— 那就是歷史最深水位。IRQ 進來後 C 處理器整段跑在
     * **SVC** 堆疊上(portASM 的 `cps #19`),所以 svc_used 才是重點。 */
    uint32_t svc_used;       /* SVC 堆疊歷史最深用量 (bytes),上限 0x2000 */
    uint32_t irq_used;       /* IRQ 堆疊歷史最深用量 (bytes),上限 0x4000 */
    uint32_t sys_used;       /* System 堆疊歷史最深用量 (bytes),上限 0x1000 */
    uint32_t stack_paint;    /* 上色成功的區塊 bitmask:1=svc 2=irq 4=sys */
    uint32_t fault_sp_svc;   /* fault 當下的 sp_svc */
    uint32_t fault_sp_irq;   /* fault 當下的 sp_irq */

    /* ---- ISR 毀損偵測(2026-08-25,version 3 起)----------------------- */
    /* portASM 在 (__SvcStackLimit-28) 或 (-32) 存了對齊修正量,只可能是
     * 0 或 4。每個 ISR 收尾時驗一次,不對就是這個 ISR 把它寫壞的。 */
    uint32_t irq_last;       /* 最後一次進入的 ICCIAR(由 pump 每 5 Hz 搬入)*/
    uint32_t irq_count;      /* ISR 進入總次數(同上)*/
    uint32_t irq_bad;        /* 偵測到毀損的次數 */
    uint32_t irq_seen;       /* 0 = 還沒抓到;1 = 下面幾欄有效 */
    uint32_t irq_culprit;    /* ★ 第一次毀損時的 ICCIAR —— 兇手 */
    uint32_t irq_w28;        /* 當時 (__SvcStackLimit-28) 的值 */
    uint32_t irq_w32;        /* 當時 (__SvcStackLimit-32) 的值 */
    uint32_t irq_sp;         /* 當時 vApplicationIRQHandler 的 sp */
    uint32_t irq_prev;       /* 前一個 ICCIAR */

    /* ---- 對齊字的自動校準(2026-08-25,version 4 起)------------------- */
    /* v3 把位址寫死成 (__SvcStackLimit-28)/(-32),但 sp_svc 的靜止值並不是
     * __SvcStackLimit,結果每個中斷都被誤判。改成由韌體自己量。 */
    uint32_t svc_rest;       /* 靜止時的 sp_svc(沒有 ISR 在跑時)*/
    uint32_t irq_word_addr;  /* 推算出的對齊字位址;0 = 還沒校準 */
    uint32_t irq_word_exp;   /* 它應該是的值(0 或 4)*/
    uint32_t irq_word_got;   /* 毀損時實際讀到的值 */

    /* ---- 漂移偵測(2026-08-25,version 5 起)--------------------------- */
    uint32_t guard_sp_min;   /* guard 自己的 sp 的最小值 */
    uint32_t guard_sp_max;   /* 同上最大值 —— 兩者不等就代表 sp_svc 會漂 */
    uint32_t rest_min;       /* 任務側量到的靜止 sp_svc 最小值 */
    uint32_t rest_max;       /* 同上最大值 */
    uint32_t fault_word;     /* ★ abort 當下對齊字的實際值(分辨器)*/
    uint32_t fault_word_addr;/* 讀的是哪個位址 */

    /* ---- 中斷巢狀深度(2026-08-25,version 6 起)----------------------- */
    /* FSP 的 bsp_common_interrupt_handler 在呼叫處理器前 __enable_irq(),
     * 所以中斷會巢狀。而 g_current_interrupt_num 只有 32 格、指標是 uint8_t
     * 且**沒有邊界檢查** —— 深度超過 32 就寫到後面的 gp_renesas_isr_context。 */
    uint32_t nest_max;       /* ★ 觀測到的最大巢狀深度(>32 就是踩到陣列外)*/
    uint32_t guard_skipped;  /* guard 在校準完成前跳過檢查的次數 */

    /* ---- v7:各深度都檢查 --------------------------------------------- */
    uint32_t irq_off;        /* 校準出來的 (對齊字位址 - guard 的 sp) */
    uint32_t irq_depth;      /* ★ 偵測到毀損時的巢狀深度 */

    /* ---- v8:故障當下的即時現場 --------------------------------------- */
    /* guard_sp_min/max 是 pump 每 5 Hz 搬過來的,故障那一刻來不及寫。
     * 下面這些由 abort handler 直接從 SRAM 讀,才是真正的最後狀態。 */
    uint32_t fault_sp_min;   /* guard 看過的最小 sp(即時)*/
    uint32_t fault_sp_max;   /* 同上最大 */
    uint32_t fault_depth;    /* abort 當下的巢狀深度 */
    uint32_t fault_irq;      /* 最後一個 icciar */
    uint32_t ring_idx;       /* 環形緩衝的下一個寫入位置 */
    /* 最後 R8_RING_N 次中斷:每格 (icciar, guard 的 sp, 巢狀深度) */
    uint32_t ring[R8_RING_N][3];

    /* ---- v9:巢狀 yield 延後(這是修正,不只是量測)------------------- */
    uint32_t yield_defer;    /* ★ 攔下多少次「巢狀中斷想切換任務」*/
    uint32_t yield_apply;    /* 回到最外層後補做的次數 */

    /* ---- v10:不經 sensor task 的獨立訊號 ------------------------------ */
    /* uptime_ticks / hb_sensor 都是 sensor task 自己搬的,它一死就看不到。
     * tick_count 由 SysTick_Handler 直接寫,是唯一能證明 tick 還活著的訊號。*/
    uint32_t tick_count;     /* ★ FreeRTOS tick 心跳(SysTick ISR 直接遞增)*/
    uint32_t sns_stage;      /* ★ sensor task 停在哪一段:
                              * 1 掏 UART  2 羅盤取樣  3 pump  4 vTaskDelay
                              * 5 i2c_wait 中 */

    /* ---- v11:IRQ 收尾三點快照 ------------------------------------------ */
    /* guard 執行時 sp 是對的、LDM 時是錯的,中間只有 POP {r2} 與 ADD sp,sp,r2
     * 會動 sp。抓這三個數字才能分辨是「sp 本來就錯」還是「pop 讀到錯的值」。 */
    uint32_t epi_sp_before;  /* POP {r2} 之前的 sp_svc */
    uint32_t epi_r2;         /* POP 出來的值(只可能是 0 或 4)*/
    uint32_t epi_sp_after;   /* ADD sp, sp, r2 之後的 sp_svc */

    /* ---- v12:排程器存活 ------------------------------------------------ */
    /* tick_count 是在 xTaskIncrementTick() **之前**遞增的,它前進只證明
     * SysTick ISR 在跑。tick_rtos 取的是 FreeRTOS 自己的 tick,
     * 只有 xTaskIncrementTick() 真的跑完才會動 —— 兩者一比就知道
     * 是「排程器壞了」還是「排程器活著但那個 task 沒被喚醒」。 */
    uint32_t tick_rtos;      /* xTaskGetTickCountFromISR(),由 tick 探針取 */

    /* ---- v13:進入端的對齊字 -------------------------------------------- */
    /* PUSH {r2} 之後立刻讀回來。與退出端的 epi_r2 一比:
     *   進入 0/4 且退出 5  -> 中途被改,兇手在兩者之間
     *   進入就是 5         -> 寫入本身有問題
     * epi_entry_addr 必須等於 epi_sp_before,否則兩次快照不是同一個框架。 */
    uint32_t epi_entry_addr; /* PUSH {r2} 後的 sp(= 對齊字的位址)*/
    uint32_t epi_entry_val;  /* 立刻讀回來的值 */

    /* ---- v14:每層框架在 guard 當下的對齊字 ------------------------------ */
    /* 與 ring[] 同一個索引。ring[] 記「哪個中斷、guard 的 sp、巢狀深度」,
     * 這裡記「guard 檢查的位址、讀到的值」——
     * 合起來就能對每一層回答「guard 當下那個字是好的嗎」,補齊時序。 */
    uint32_t ring_word[R8_RING_N][2];   /* [0]=位址  [1]=值 */

    /* ---- v15:context switch 走了哪條路 ---------------------------------- */
    /* 出事框架的 guard 從沒跑過、收尾卻執行了 => 控制流跳進來的。
     * 唯一會這樣的是 portRESTORE_CONTEXT 的 RFEIA。這幾個計數器指出
     * 切換從哪條路徑發生,以及有沒有在巢狀狀態下發生過(那是致命的)。 */
    uint32_t sw_exit_cnt;    /* switch_before_exit 被走到的次數 */
    uint32_t sw_exit_nest;   /* ★ 其中觀測到的最大 ulPortInterruptNesting */
    uint32_t sw_svc_cnt;     /* SVC_Handler(portYIELD)的次數 */
    uint32_t sw_svc_nest;    /* ★ 同上最大巢狀層數 */

    /* ---- v16:每層框架的 IRQ 模式返回狀態 -------------------------------- */
    /* portASM 進入時 push {lr}(返回位址)再 push {lr}(SPSR),
     * 所以 sp_irq 指向 SPSR、sp_irq+4 是返回位址。
     * SPSR 的低 5 位是模式:0x13=SVC(打斷了另一個 ISR,巢狀的正常樣子)、
     * 0x1F=SYS(打斷了任務)。返回位址就是 MOVS pc, lr 會跳去的地方。 */
    uint32_t ring_irq[R8_RING_N][3];   /* [0]=sp_irq [1]=保存的SPSR [2]=返回位址 */

    /* ---- v17:收尾當下中斷是否真的遮蔽 ----------------------------------- */
    /* 收尾的前提是中斷已遮蔽。若 __enable_irq()/__disable_irq() 有路徑不平衡,
     * 外層的收尾就會被搶佔 —— 那正好能解釋「收尾的 sp 等於巢狀發生當下
     * 外層 ISR 的 sp_svc」這個觀測。unmasked 只要出現一次就是抓到了。 */
    uint32_t epi_cpsr;       /* 收尾開頭讀到的 CPSR */
    uint32_t epi_unmasked;   /* ★ 收尾時 I=0(中斷未遮蔽)的累計次數 */

    /* ---- v18:收尾屬於哪個中斷 ------------------------------------------ */
    /* r4 在收尾處仍存著 ICCIAR。與最後一筆 ring 的 ICCIAR 一比:
     *   相同 -> 同一個中斷,sp 真的多了 32 bytes
     *   不同 -> 收尾快照與最後那筆 guard 不是同一件事,先前的推理前提就錯了 */
    uint32_t epi_icciar;     /* ★ 收尾當下的 ICCIAR(來自 r4)*/

    /* ---- v19:向量表索引越界(★ 根因)------------------------------------ */
    /* bsp_common_interrupt_handler 用 gic_intid 索引 g_vector_table 沒有邊界檢查,
     * 而表只有 BSP_VECTOR_TABLE_MAX_ENTRIES 格、GIC 的 INTID 空間到 1022。
     * 實測抓到 ICCIAR=876 -> 越界讀到 g_gpt5_extend 裡的 0x00010001
     * -> 當函式指標呼叫 -> 跳進 ITCM 0x10000 執行任意內容。 */
    uint32_t bad_intid_cnt;  /* ★ 越界的 INTID 出現次數 */
    uint32_t bad_intid_first;/* 第一個越界的 INTID */
    uint32_t bad_intid_last; /* 最後一個 */
    uint32_t bad_intid_max;  /* 觀測到的最大 INTID(含界內)*/

    /* ---- v21:巢狀事件累計 ---------------------------------------------- */
    /* lr 修復的通過標準是「累積 >= 1e5 次巢狀事件且零故障」——
     * nest_max 只記最大深度,判不了這個。 */
    uint32_t nest_events;    /* ★ depth >= 2 的進入次數 */
} r8_blackbox_t;

#define R8_BB   ((volatile r8_blackbox_t *) R8_BB_ADDR)

/* 計數器遞增。這些只是統計,不需要原子性 ——
 * 少算一次無所謂,重點是「有沒有在動」。 */
#define R8_BB_INC(field)                                       \
    do {                                                       \
        if (R8_BB_MAGIC == R8_BB->magic) { R8_BB->field++; }   \
    } while (0)

#define R8_BB_SET(field, val)                                  \
    do {                                                       \
        if (R8_BB_MAGIC == R8_BB->magic) { R8_BB->field = (uint32_t)(val); } \
    } while (0)

void r8_blackbox_init(void);
void r8_stack_paint(void);
void r8_stack_scan(void);
void r8_irq_guard(uint32_t icciar);
void r8_irq_guard_calibrate(void);
void r8_nest_probe(uint32_t depth);
void r8_bad_intid(uint32_t intid);
extern volatile uint32_t g_r8_epi[8];
void r8_tick_probe(void);
#define R8_SNS_STAGE(n)  R8_BB_SET(sns_stage, (n))
uint32_t r8_read_mode_sp(uint32_t mode_byte);
#define R8_BB_STAGE(n)   R8_BB_SET(boot_stage, (n))
void r8_blackbox_fault(uint32_t type, uint32_t a0, uint32_t a1,
                       uint32_t lr, const char * task);

#endif /* R8_BLACKBOX_H */
