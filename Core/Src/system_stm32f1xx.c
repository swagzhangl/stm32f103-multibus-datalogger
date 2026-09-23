/* ============================================================
 * system_stm32f1xx.c —— 本项目自写的 system 文件（替换 ST 的 HAL 版本）
 *
 * 【为什么不用 ST 给的那份】
 *   ST 的 system_stm32f1xx.c 里 SystemInit() 会去读一堆
 *   RCC 相关的宏（HSE_VALUE、PLL 参数等），那些宏来自 HAL 的
 *   配置头；而本项目决策 1 是【不用 HAL】。与其把 HAL 的头文件
 *   拖进来只为了两个函数，不如自己写 —— 总共不到 40 行，
 *   而且改时钟时"谁在改、改了什么"一目了然。
 *
 * 【启动流程里它是被谁调用的】
 *   看 Core/Startup/startup_stm32f103xb.s 的 Reset_Handler：
 *       bl  SystemInit            ← 复位后第一个 C 函数
 *       拷贝 .data 段（初值从 Flash 搬到 RAM）
 *       清零 .bss 段
 *       bl  __libc_init_array
 *       bl  main
 *   ⇒ SystemInit 执行时，.data 段还没初始化，所以它
 *     【绝对不能依赖任何有初值的全局变量】。
 * ============================================================ */
#include "stm32f1xx.h"

/* 当前内核时钟。复位后是 HSI 8 MHz；
 * clock_init_72mhz() 成功后被改写成 72 MHz。 */
uint32_t SystemCoreClock = 8000000U;

/* AHB/APB 分频比，保留这两个变量是为了兼容
 * 某些库（如 ST 的 SysTick 配置）可能引用它们。 */
const uint8_t AHBPrescTable[16]  = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                    1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U};
const uint8_t APBPrescTable[8]   = {0U, 0U, 0U, 0U, 1U, 2U, 3U, 4U};

/* ============================================================
 * 复位后最早执行的初始化 —— 只做「不依赖任何全局变量」的事
 * ============================================================ */
void SystemInit(void)
{
    /* ① 把中断向量表放到 Flash 起始地址。
     *    本工程没有 Bootloader，值为 0x08000000，和复位默认一致；
     *    显式写一次是为了将来加 IAP 时这一行代码的含义是明确的。 */
    SCB->VTOR = FLASH_BASE;

    /* ② 打开 Flash 预取缓冲。
     *    注意这里【不改等待周期】—— 此刻还在 8 MHz，
     *    等待周期由 clock_init_72mhz() 在提频前统一设置。 */
    FLASH->ACR |= FLASH_ACR_PRFTBE;

    /* ③ 清掉复位来源标志里我们不关心的位。
     *    RCC->CSR 的 RMVF 位写 1 清除所有复位标志，
     *    这样后面读 CSR 得到的就是"本次运行期间"的复位原因，
     *    不会被上一次上电的标志污染。 */
    RCC->CSR |= RCC_CSR_RMVF;
}

/* ============================================================
 * 按寄存器实际值重算 SystemCoreClock
 *
 * 本工程的时钟树由 clock_init_72mhz() 独占配置，
 * 所以这里直接把结果写成 72 MHz 即可 —— 需要"反算校验"时，
 * 用 clock_get_sysclk_hz()，那个函数是从寄存器反推的，两者互证。
 * ============================================================ */
void SystemCoreClockUpdate(void)
{
    SystemCoreClock = 72000000U;
}
