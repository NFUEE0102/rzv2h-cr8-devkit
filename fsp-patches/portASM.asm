;/*
; * FreeRTOS Kernel V10.4.3 LTS Patch 2
; * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
; *
; * Permission is hereby granted, free of charge, to any person obtaining a copy of
; * this software and associated documentation files (the "Software"), to deal in
; * the Software without restriction, including without limitation the rights to
; * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
; * the Software, and to permit persons to whom the Software is furnished to do so,
; * subject to the following conditions:
; *
; * The above copyright notice and this permission notice shall be included in all
; * copies or substantial portions of the Software.
; *
; * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
; * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
; * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
; * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
; * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
; *
; * http://www.FreeRTOS.org
; * https://github.com/FreeRTOS
; *
; */

#if (defined(__VFP_FP__) && !defined(__SOFTFP__))
/* Use FPU context for all tasks */
#define configUSE_TASK_FPU_SUPPORT 2
#else
/* Disable the FPU */
#define configUSE_TASK_FPU_SUPPORT 0
#endif

    .text
    .arm

    .set SYS_MODE,  0x1f
    .set SVC_MODE,  0x13
    .set IRQ_MODE,  0x12

    /* Variables and functions. */
    .extern pxCurrentTCB
    .extern vTaskSwitchContext
    .extern FreeRTOS_GetActiveIRQ
    .extern vApplicationIRQHandler
    .extern FreeRTOS_EndOfInterrupt
    .extern ulPortTaskHasFPUContext
    .extern ulCriticalNesting
    .extern ulPortInterruptNesting
    .extern g_r8_epi
    .extern g_r8_sw

    .global IRQ_Handler
    .global SVC_Handler
    .global vPortRestoreTaskContext

.macro portSAVE_CONTEXT

    /* Save the LR and SPSR onto the system mode stack before switching to
    system mode to save the remaining system mode registers. */
    SRSDB   sp!, #SYS_MODE
    CPS     #SYS_MODE
    PUSH    {R0-R12, R14}

    /* Push the critical nesting count. */
    LDR     R2, =ulCriticalNesting
    LDR     R1, [R2]
    PUSH    {R1}

#if (configUSE_TASK_FPU_SUPPORT != 0)
    /* Does the task have a floating point context that needs saving?  If
    ulPortTaskHasFPUContext is 0 then no. */
    LDR     R2, =ulPortTaskHasFPUContext
    LDR     R3, [R2]
    CMP     R3, #0

    /* Save the floating point context, if any. */
    FMRXNE  R1,  FPSCR
    VPUSHNE {D0-D15}
#if configFPU_D32 == 1
    VPUSHNE {D16-D31}
#endif /* configFPU_D32 */
    PUSHNE  {R1}

    /* Save ulPortTaskHasFPUContext itself. */
    PUSH    {R3}
#endif /* (configUSE_TASK_FPU_SUPPORT != 0) */

    /* Save the stack pointer in the TCB. */
    LDR     R0, =pxCurrentTCB
    LDR     R1, [R0]
    STR     SP, [R1]

    .endm

; /**********************************************************************/

.macro portRESTORE_CONTEXT

    /* Set the SP to point to the stack of the task being restored. */
    LDR     R0, =pxCurrentTCB
    LDR     R1, [R0]
    LDR     SP, [R1]

#if (configUSE_TASK_FPU_SUPPORT != 0)
    /* Is there a floating point context to restore?  If the restored
    ulPortTaskHasFPUContext is zero then no. */
    LDR     R0, =ulPortTaskHasFPUContext
    POP     {R1}
    STR     R1, [R0]
    CMP     R1, #0

    /* Restore the floating point context, if any. */
    POPNE   {R0}
#if configFPU_D32 == 1
    VPOPNE  {D16-D31}
#endif /* configFPU_D32 */
    VPOPNE  {D0-D15}
    VMSRNE  FPSCR, R0
#endif /* (configUSE_TASK_FPU_SUPPORT != 0) */

    /* Restore the critical section nesting depth. */
    LDR     R0, =ulCriticalNesting
    POP     {R1}
    STR     R1, [R0]

    /* Restore all system mode registers other than the SP (which is already
    being used). */
    POP     {R0-R12, R14}

    /* Return to the task code, loading CPSR on the way. */
    RFEIA   sp!

    .endm




/******************************************************************************
 * SVC handler is used to start the scheduler.
 *****************************************************************************/
.align 4
.type SVC_Handler, %function
SVC_Handler:
    /* Save the context of the current task and select a new task to run. */
    portSAVE_CONTEXT
    LDR R0, =vTaskSwitchContext
    BLX R0

    /* 計數(2026-08-27 診斷用)。放在 BLX 之後 —— SVC_Handler 進來時
       所有暫存器都還是被中斷任務的,一個都不能碰;而 C 呼叫回來後
       r0-r3 依 AAPCS 是 scratch,r4 接下來也會被 portRESTORE_CONTEXT
       從堆疊覆寫。 */
    LDR     r3, =g_r8_sw
    LDR     r2, [r3, #8]
    ADD     r2, r2, #1
    STR     r2, [r3, #8]
    LDR     r2, =ulPortInterruptNesting
    LDR     r2, [r2]
    LDR     r4, [r3, #12]
    CMP     r2, r4
    STRHI   r2, [r3, #12]

    portRESTORE_CONTEXT


/******************************************************************************
 * vPortRestoreTaskContext is used to start the scheduler.
 *****************************************************************************/
.type vPortRestoreTaskContext, %function
vPortRestoreTaskContext:
    /* Switch to system mode. */
    CPS     #SYS_MODE
    DSB
    ISB
    portRESTORE_CONTEXT

.align 4
.type IRQ_Handler, %function
IRQ_Handler:

    /* Return to the interrupted instruction. */
    SUB     lr, lr, #4

    /* Push the return address and SPSR. */
    PUSH    {lr}
    MRS     lr, SPSR
    PUSH    {lr}

    /* Change to supervisor mode to allow reentry. */
    CPS     #SVC_MODE

    /* Push used registers.
       ⚠️ 2026-08-27 修:lr(lr_svc)必須一併保存。原本只 PUSH {r0-r4, r12},
       其後的 BLX 會覆寫 lr_svc。非巢狀時被打斷的任務在 SYS 模式(lr banked)
       無感;**巢狀時被打斷的外層 ISR 就在 SVC 模式** —— 外層恢復後以 lr 返回
       時會跳進本層留下的位址(= BLX FreeRTOS_EndOfInterrupt 的下一條 = 收尾),
       帶著錯的 sp 執行 POP/LDM 而 Data Abort。上游 FreeRTOS 的各 ARM port
       都有保護 lr(直接入棧或以模式隔離)。 */
    PUSH    {r0-r4, r12, lr}

    /* Increment the interrupt nesting count.  Upstream FreeRTOS keeps this in
    r1/r3/r4 across the C handler call, but this port has no PUSH {r0-r3, lr}
    around that call and r4 already carries the ICCIAR, so instead we simply
    increment here and re-read/decrement on the way out.  r2 is dead (it is
    overwritten by the MOV below) and r3 is not live yet. */
    LDR     r3, =ulPortInterruptNesting
    LDR     r2, [r3]
    ADD     r2, r2, #1
    STR     r2, [r3]

    /* Ensure bit 2 of the stack pointer is clear.  r2 holds the bit 2 value for
    future use.  */
    MOV     r2, sp
    AND     r2, r2, #4
    SUB     sp, sp, r2
    PUSH    {r2}

    /* 進入端快照(2026-08-27 診斷用):把剛存進去的對齊字讀回來。
       r0-r3 在此處都是死的 —— 上面的 PUSH {r0-r4, r12} 已經把它們存起來,
       而下一道 BLX 也會把它們當 scratch。 */
    LDR     r3, =g_r8_epi
    STR     sp, [r3, #12]       /* 對齊字的位址 */
    LDR     r1, [sp]
    STR     r1, [r3, #16]       /* 立刻讀回來的值 */

    BLX     FreeRTOS_GetActiveIRQ
    MOV     r4, r0

    BLX     vApplicationIRQHandler

    CPSID   i
    DSB
    ISB

    MOV     r0, r4
    BLX     FreeRTOS_EndOfInterrupt

    /* Restore spack pointer */
    /* 收尾三點快照(2026-08-27 診斷用)。r0-r3 在此處都是死的:剛從
       FreeRTOS_EndOfInterrupt 回來,而兩條退出路徑接下來都會
       POP {r0-r4, r12} 整組還原。 */
    LDR     r3, =g_r8_epi
    /* 中斷真的遮蔽了嗎?I 位元是 bit 7。若曾經是 0,就代表收尾會被搶佔。 */
    MRS     r1, cpsr
    STR     r1, [r3, #20]
    TST     r1, #0x80
    LDREQ   r1, [r3, #24]
    ADDEQ   r1, r1, #1
    STREQ   r1, [r3, #24]
    STR     r4, [r3, #28]       /* 這個收尾屬於哪個中斷(r4 仍存著 ICCIAR)*/
    STR     sp, [r3, #0]        /* POP 之前的 sp */
    POP     {r2}
    STR     r2, [r3, #4]        /* POP 出來的值 —— 只可能是 0 或 4 */
    ADD     sp, sp, r2
    STR     sp, [r3, #8]        /* ADD 之後的 sp */

    /* Decrement the interrupt nesting count.  FreeRTOS_EndOfInterrupt has
    returned, so r0-r3 are dead here (both exit paths below restore the whole
    set with POP {r0-r4, r12}).  This must come *before* the branch to
    switch_before_exit, because that path runs portSAVE_CONTEXT and never
    comes back. */
    LDR     r1, =ulPortInterruptNesting
    LDR     r0, [r1]
    SUB     r0, r0, #1
    STR     r0, [r1]

    /* A context switch is never performed if the nesting count is not 0.
    Doing so would save the interrupted context -- an outer ISR still running
    in SVC mode -- as if it were a task. */
    CMP     r0, #0
    BNE     exit_without_switch

    /* Did the interrupt request a context switch?  r1 holds the address of
    ulPortYieldRequired and r0 the value of ulPortYieldRequired for future
    use. */
    LDR     r1, =ulPortYieldRequired
    LDR     r0, [r1]
    CMP     r0, #0
    BNE     switch_before_exit

exit_without_switch:
    /* No context switch.  Restore used registers, LR_irq and SPSR before
    returning. */
    POP     {r0-r4, r12, lr}
    CPS     #IRQ_MODE
    POP     {LR}
    MSR     SPSR_cxsf, LR
    POP     {LR}
    MOVS    PC, LR

switch_before_exit:
    /* 計數(2026-08-27 診斷用)。r2/r3/r4 緊接著會被下面的
       POP {r0-r4, r12} 整組還原,所以在這裡用它們是安全的;
       r0/r1 不能碰 —— r1 是 &ulPortYieldRequired,下一道就要用。 */
    LDR     r3, =g_r8_sw
    LDR     r2, [r3]
    ADD     r2, r2, #1
    STR     r2, [r3]
    LDR     r2, =ulPortInterruptNesting
    LDR     r2, [r2]
    LDR     r4, [r3, #4]
    CMP     r2, r4
    STRHI   r2, [r3, #4]

    /* A context swtich is to be performed.  Clear the context switch pending
    flag. */
    MOV     r0, #0
    STR     r0, [r1]

    /* Restore used registers, LR-irq and SPSR before saving the context
    to the task stack. */
    POP     {r0-r4, r12, lr}
    CPS     #IRQ_MODE
    POP     {LR}
    MSR     SPSR_cxsf, LR
    POP     {LR}
    portSAVE_CONTEXT

    /* Call the function that selects the new task to execute.
    vTaskSwitchContext() if vTaskSwitchContext() uses LDRD or STRD
    instructions, or 8 byte aligned stack allocated data.  LR does not need
    saving as a new LR will be loaded by portRESTORE_CONTEXT anyway. */
    LDR     R0, =vTaskSwitchContext
    BLX     R0

    /* Restore the context of, and branch to, the task selected to execute
    next. */
    portRESTORE_CONTEXT

vApplicationIRQHandlerConst: .word vApplicationIRQHandler

.end





