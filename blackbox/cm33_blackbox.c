/* CM33 黑盒子實作 —— 見 cm33_blackbox.h 的佈局鐵律。 */
#include "cm33_blackbox.h"
#include "FreeRTOS.h"
#include "task.h"

/* ARMv8-M SCB fault 暫存器(S 視角;直接位址,免 CMSIS 標頭名稱糾葛) */
#define SCB_CFSR   (*(volatile uint32_t *) 0xE000ED28u)
#define SCB_HFSR   (*(volatile uint32_t *) 0xE000ED2Cu)
#define SCB_MMFAR  (*(volatile uint32_t *) 0xE000ED34u)
#define SCB_BFAR   (*(volatile uint32_t *) 0xE000ED38u)

void cm33_bb_init (void)
{
    cm33_bb_t * b = CM33_BB;

    if (CM33_BB_MAGIC != b->magic)
    {
        /* 冷 DDR / 前任是別的佈局:整塊清掉重來 */
        volatile uint32_t * p = (volatile uint32_t *) b;
        for (uint32_t i = 0; i < sizeof(*b) / 4u; i++)
        {
            p[i] = 0u;
        }
        b->magic   = CM33_BB_MAGIC;
        b->version = CM33_BB_VER;
    }
    b->boot_count  += 1u;
    b->fault_type   = 0u;
    b->uptime_ticks = 0u;
    b->boot_stage   = 1u;
}

void cm33_bb_pump (void)
{
    TickType_t t = xTaskGetTickCount();
    CM33_BB->uptime_ticks = (uint32_t) t;
    CM33_BB->tick_count   = (uint32_t) t;
}

/* ---- HardFault 捕捉 ------------------------------------------------------
 * naked 進入點抓 MSP 與 EXC_RETURN,C 側依 EXC_RETURN bit2 選 MSP/PSP 框。
 * demo 未使能 MemManage/BusFault/UsageFault(SHCSR),一切都升級成 HardFault,
 * 所以覆蓋這一個(S/NS 兩符號)就攔得全。 */
void cm33_bb_hardfault(uint32_t * msp_frame, uint32_t exc_return);

void cm33_bb_hardfault (uint32_t * msp_frame, uint32_t exc_return)
{
    cm33_bb_t * b = CM33_BB;
    uint32_t  * f = msp_frame;

    if (exc_return & 0x4u)               /* bit2:框在 PSP */
    {
        uint32_t psp;
        __asm volatile ("mrs %0, psp" : "=r" (psp));
        f = (uint32_t *) psp;
    }

    b->cfsr       = SCB_CFSR;
    b->hfsr       = SCB_HFSR;
    b->mmfar      = SCB_MMFAR;
    b->bfar       = SCB_BFAR;
    b->stacked_lr = f[5];
    b->stacked_pc = f[6];
    b->exc_return = exc_return;
    b->fault_type = 3u;                  /* 與 R8 版語義對齊:3 = 資料/硬體中止 */

    for ( ; ; )
    {
        /* 停在這裡,狀態保留給 A55 讀 */
    }
}

__attribute__((naked)) void HardFault_Handler_S (void)
{
    __asm volatile (
        "mrs r0, msp        \n"
        "mov r1, lr         \n"
        "b   cm33_bb_hardfault\n");
}

__attribute__((naked)) void HardFault_Handler_NS (void)
{
    __asm volatile (
        "mrs r0, msp        \n"
        "mov r1, lr         \n"
        "b   cm33_bb_hardfault\n");
}
