/* ============================================================
 * stm32f1xx_it.c —— 异常与中断向量实现（寄存器版）
 *
 * ⚠️⚠️ 本文件【绝对不可以】定义下面三个函数 ⚠️⚠️
 *
 *       SVC_Handler
 *       PendSV_Handler
 *       SysTick_Handler
 *
 *   因为 FreeRTOSConfig.h 里有这三行映射：
 *       #define vPortSVCHandler       SVC_Handler
 *       #define xPortPendSVHandler    PendSV_Handler
 *       #define xPortSysTickHandler   SysTick_Handler
 *   于是 FreeRTOS 的 port.c 会以这三个名字定义函数。
 *   本文件若再定义一次 → 链接期报
 *       multiple definition of `PendSV_Handler'
 *
 *   这是"不用 CubeMX、自己移植 FreeRTOS"时最经典的翻车点：
 *   CubeMX 生成的 stm32f1xx_it.c 里默认带 SysTick_Handler，
 *   而 CubeMX 那版是靠 cmsis_os2.c 的宏绕开的；本工程走了原生 API，
 *   绕开机制不存在了，所以必须手动记住"这三个别写"。
 *
 *   USART1_IRQHandler 也不在这里 —— 它写在 uart_dbg.c，
 *   和串口驱动放在一起（谁用谁管，改串口时不用两处跳）。
 * ============================================================ */
#include "stm32f1xx.h"
#include "uart_dbg.h"

/* ============================================================
 * HardFault 现场提取
 *
 * 【为什么值得写这 20 行】
 *   默认的 HardFault_Handler 就是一个 while(1)，跑飞了之后
 *   你只看到"程序不动了"，完全不知道死在哪。
 *
 *   而 Cortex-M3 在进入异常时会【自动压栈】8 个寄存器
 *   （R0-R3, R12, LR, PC, xPSR）—— 其中 PC 就是"出错时正在执行
 *   的那条指令地址"。把它打出来，就能在 .map / 反汇编里定位到具体函数。
 *
 *   判断用哪个栈：LR 的 bit2 为 0 表示异常前用的是主栈 MSP，
 *   为 1 表示用的是任务栈 PSP（FreeRTOS 下正常运行都是 PSP）。
 *
 * 【面试价值】
 *   问"程序跑飞了你怎么查"，
 *   答"打印复位原因 + 从异常栈帧取 PC 定位到指令"，
 *   比答"加打印慢慢试"高一个层次。
 * ============================================================ */
void hard_fault_report(uint32_t *frame)
{
    uart_dbg_str("\r\n\r\n!!!!!! HARD FAULT !!!!!!\r\n");
    uart_dbg_str("R0 =0x"); uart_dbg_hex32(frame[0]);  uart_dbg_str("\r\n");
    uart_dbg_str("R1 =0x"); uart_dbg_hex32(frame[1]);  uart_dbg_str("\r\n");
    uart_dbg_str("R2 =0x"); uart_dbg_hex32(frame[2]);  uart_dbg_str("\r\n");
    uart_dbg_str("R3 =0x"); uart_dbg_hex32(frame[3]);  uart_dbg_str("\r\n");
    uart_dbg_str("R12=0x"); uart_dbg_hex32(frame[4]);  uart_dbg_str("\r\n");
    uart_dbg_str("LR =0x"); uart_dbg_hex32(frame[5]);  uart_dbg_str("\r\n");
    uart_dbg_str("PC =0x"); uart_dbg_hex32(frame[6]);  uart_dbg_str("  <-- 出错指令地址\r\n");
    uart_dbg_str("xPSR=0x"); uart_dbg_hex32(frame[7]); uart_dbg_str("\r\n");

    /* 寄存器层面的常见原因（按出现频率排）：
     *   · 取指/取数地址不是合法区域  → CFSR 的 IBUSERR/PRECISERR
     *   · 非对齐访问（F103 不支持部分非对齐）→ UFSR 的 UNALIGNED
     *   · 除零（要开 CCR.DIV_0_TRP 才会触发）
     *   · 跳到 0 或非法函数指针 → PC 会是 0x00000000 之类
     * 想看更细的原因就读 SCB->CFSR / SCB->HFSR，本版先给到 PC 就够定位。 */
    uart_dbg_str("CFSR =0x"); uart_dbg_hex32(SCB->CFSR); uart_dbg_str("\r\n");
    uart_dbg_str("HFSR =0x"); uart_dbg_hex32(SCB->HFSR); uart_dbg_str("\r\n");
    uart_dbg_str("用 PC 值去 .map 文件里查落在哪个函数\r\n");

    for (;;) { }
}

/* ============================================================
 * HardFault_Handler
 *
 * naked 属性 = 编译器不要给我生成任何函数入口代码（不压栈、不改寄存器），
 * 整个函数体就是下面这几条汇编。
 *
 *   ① tst lr, #4   —— 看 EXC_RETURN 的 bit2
 *   ② mrs r0, msp / psp —— bit2=0 说明异常前用主栈，=1 说明用任务栈。
 *      无论是哪个，把栈指针作为第一个参数传给 C 函数。
 *   ③ 尾调用的 b（而不是 bl）跳到 C 函数：
 *      反正它不会返回，不需要保存返回地址。
 * ============================================================ */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile (
        "tst lr, #4            \n"
        "ite eq                \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "b hard_fault_report   \n"
    );
}

void NMI_Handler(void)          { for (;;) { } }
void MemManage_Handler(void)    { for (;;) { } }
void BusFault_Handler(void)     { for (;;) { } }
void UsageFault_Handler(void)   { for (;;) { } }
void DebugMon_Handler(void)     { }
