/* ============================================================
 * clock_init.c —— 时钟树：HSI 8 MHz → HSE 8 MHz × PLL9 = 72 MHz
 *
 * 【为什么这一步是 W1 最容易翻车的地方】
 *   三个坑，每个都能让人查一整天：
 *
 *   ① FLASH->ACR 等待周期忘了配（或配晚）
 *      Flash 的读取速度跟不上 72 MHz 的取指速度，必须插入等待周期。
 *      0 等待周期在 72 MHz 下会"随机跑飞"——现象很诡异：
 *      能下载、能跑几秒、然后不知跳到哪去了。
 *      ⇒ 必须在【提高频率之前】就配好 2 个等待周期并打开预取缓冲。
 *
 *   ② APB1 分频忘了配成 /2
 *      APB1 的时钟上限就是 36 MHz。复位后 PPRE1 默认是 /1，
 *      如果直接切到 72 MHz，APB1 就变成 72 MHz —— 超频 2 倍。
 *      现象是"外设时好时坏"。⇒ 必须在切 SYSCLK 之前配好分频。
 *
 *   ③ 切完不确认
 *      切 SYSCLK 是异步操作，必须回读 SWS 位等它真的切过去。
 *      不等就继续跑，后面所有基于 72 MHz 的计算（BRR、延时）全是错的。
 * ============================================================ */
#include "stm32f1xx.h"
#include "board.h"
#include "clock_init.h"

/* 超时计数：循环体在 8 MHz 下每次约几百个周期。
 * 取 0x80000 次 ≈ 0.1 秒量级 —— 远大于正常起振时间（8 MHz 晶振典型 < 5 ms），
 * 又能在晶振坏了的时候及时退出而不是死等。 */
#define CLOCK_TIMEOUT   0x80000UL

uint8_t clock_init_72mhz(void)
{
    uint32_t t;

    /* ============================================================
     * ① Flash 等待周期 + 预取 —— 必须在提频之前
     *
     * LATENCY = 2  （等待周期数）
     * PRFTBE  = 1  （预取缓冲使能）
     *
     * 为什么是 2：按 RM0008 Table 4，48 < SYSCLK ≤ 72 MHz 时要求 2 个等待周期。
     * ============================================================ */
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_2) { }

    /* ============================================================
     * ② 开 HSE（外部 8 MHz 晶振），等它稳定
     * ============================================================ */
    RCC->CR |= RCC_CR_HSEON;
    t = CLOCK_TIMEOUT;
    while (((RCC->CR & RCC_CR_HSERDY) == 0U) && (t != 0U)) { t--; }
    if (t == 0U) { return 1U; }                       /* 晶振没起振 */

    /* ============================================================
     * ③ 配 PLL：源 = HSE（不分频），倍频 = ×9
     *
     * PLLMULL 编码是"实际倍数 - 2"的偏移码：0000=×2 … 0111=×9 … 1110/1111=×16
     * 8 MHz × 9 = 72 MHz
     * ============================================================ */
    RCC->CFGR &= ~(RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL);
    RCC->CFGR |=  (RCC_CFGR_PLLSRC                  /* PLL 源选 HSE */
                   | RCC_CFGR_PLLMULL9);            /* ×9 */

    RCC->CR |= RCC_CR_PLLON;
    t = CLOCK_TIMEOUT;
    while (((RCC->CR & RCC_CR_PLLRDY) == 0U) && (t != 0U)) { t--; }
    if (t == 0U) { return 2U; }                       /* PLL 没锁住 */

    /* ============================================================
     * ④ 总线分频 —— 必须在切 SYSCLK 之前
     *
     *   HPRE  = /1  → AHB  =  72 MHz
     *   PPRE1 = /2  → APB1 =  36 MHz   ← 上限就是 36 MHz，不能给 /1
     *   PPRE2 = /1  → APB2 =  72 MHz   ← USART1 挂在这条总线上
     *
     * （先把三个域清零再写值，因为复位值是 /1，不带这步的话 PPRE1 就留在 /1 了）
     * ============================================================ */
    RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    RCC->CFGR |=  RCC_CFGR_PPRE1_DIV2;

    /* ============================================================
     * ⑤ 切换 SYSCLK 到 PLL，并回读 SWS 确认真的切过去了
     * ============================================================ */
    RCC->CFGR &= ~RCC_CFGR_SW;
    RCC->CFGR |=  RCC_CFGR_SW_PLL;
    t = CLOCK_TIMEOUT;
    while (((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) && (t != 0U)) { t--; }
    if (t == 0U) { return 3U; }

    /* ============================================================
     * ⑥ 告诉全世界"现在是 72 MHz 了"
     *
     * SystemCoreClock 被三处依赖：
     *   · FreeRTOS 的 configCPU_CLOCK_HZ（决定 tick 长度）
     *   · delay_us 的 DWT 每微秒周期数标定
     *   不更新它 → tick 长度和延时全错，而且错得很隐蔽。
     *
     * 关掉 HSI 可以省一点功耗，但保留它有个好处：
     * 万一 HSE 出问题还能当备用时钟源。这里选择保留。
     * ============================================================ */
    SystemCoreClock = BOARD_SYSCLK_HZ;

    return 0U;
}

/* ============================================================
 * 回读真实 SYSCLK
 *
 * 【为什么不能直接 return SystemCoreClock】
 *   那是"我以为的频率"。本函数是"从寄存器里反算出来的频率"，
 *   两者互为校验 —— 自检里把两个数都打出来比对，
 *   才能证明时钟树真的配对了，而不是变量被赋了个漂亮的值。
 * ============================================================ */
uint32_t clock_get_sysclk_hz(void)
{
    uint32_t sws = RCC->CFGR & RCC_CFGR_SWS;
    uint32_t sysclk;

    if (sws == RCC_CFGR_SWS_PLL)
    {
        uint32_t raw    = (RCC->CFGR & RCC_CFGR_PLLMULL) >> RCC_CFGR_PLLMULL_Pos;
        /* PLLMULL 是偏移码：0000=×2 … 1110/1111=×16 */
        uint32_t mul    = (raw >= 14U) ? 16U : (raw + 2U);
        uint32_t use_hse = ((RCC->CFGR & RCC_CFGR_PLLSRC) != 0U) ? 1U : 0U;

        if (use_hse != 0U)
        {
            /* PLLXTPRE = 0 → HSE 不分频 */
            uint32_t div = ((RCC->CFGR & RCC_CFGR_PLLXTPRE) != 0U) ? 2U : 1U;
            sysclk = (BOARD_HSE_HZ / div) * mul;
        }
        else
        {
            sysclk = (8000000UL / 2UL) * mul;   /* HSI/2 = 4 MHz */
        }
    }
    else if (sws == RCC_CFGR_SWS_HSE)
    {
        sysclk = BOARD_HSE_HZ;
    }
    else
    {
        sysclk = 8000000UL;                     /* HSI 默认值 */
    }

    return sysclk;
}

/* ============================================================
 * 分频比换算（RM0008 的编码表）
 *
 * HPRE（AHB）：0xxx → /1；1000 → /2；1001 → /4；1010 → /8；
 *              1011 → /16；1100 → /64；1101 → /128；1110 → /256；1111 → /512
 * PPRE（APB）：0xx  → /1；100  → /2；101  → /4；110  → /8；111  → /16
 * ============================================================ */
static uint32_t hpre_div(uint32_t hpre)
{
    if (hpre < 8U) { return 1U; }

    switch (hpre)
    {
    case 8U:  return 2U;
    case 9U:  return 4U;
    case 10U: return 8U;
    case 11U: return 16U;
    case 12U: return 64U;
    case 13U: return 128U;
    case 14U: return 256U;
    default:  return 512U;
    }
}

/* APB 的分频编码正好是 2 的幂偏移：4→2, 5→4, 6→8, 7→16 */
static uint32_t ppre_div(uint32_t ppre)
{
    if (ppre < 4U) { return 1U; }
    return (1UL << (ppre - 3U));
}

uint32_t clock_get_hclk_hz(void)
{
    uint32_t hpre = (RCC->CFGR & RCC_CFGR_HPRE) >> RCC_CFGR_HPRE_Pos;
    return clock_get_sysclk_hz() / hpre_div(hpre);
}

uint32_t clock_get_pclk1_hz(void)
{
    uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
    return clock_get_hclk_hz() / ppre_div(ppre1);
}

uint32_t clock_get_pclk2_hz(void)
{
    uint32_t ppre2 = (RCC->CFGR & RCC_CFGR_PPRE2) >> RCC_CFGR_PPRE2_Pos;
    return clock_get_hclk_hz() / ppre_div(ppre2);
}
