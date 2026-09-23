/* ============================================================
 * buzzer.c —— 有源蜂鸣器驱动（PB0）+ 回差报警判定
 *
 * 【有源 vs 无源】
 *   有源：内部自带振荡电路，给直流电平就发声（本模块就是这种）
 *   无源：内部只有线圈，必须给它 2~4 kHz 方波才发声
 *   本驱动只适用于"有源"。
 *
 * 【为什么用 BSRR/BRR 而不是 ODR】
 *   ODR 要「读-改-写」三步，中断里被打断会丢状态。
 *   BSRR/BRR 是「写 1 生效」的原子寄存器，一条语句搞定。
 * ============================================================ */
#include "stm32f1xx.h"
#include "buzzer.h"
#include "board.h"

/* ------------------------------------------------------------
 * 触发极性
 *   大多数 3 针模块是「低电平触发」（I/O 拉低才响）。
 *   判断方法：拿一根杜邦线把 I/O 直接碰 GND 或 3V3，看哪种会响。
 *      碰 GND 就响 → 低触发 → 本行保持 1
 *      碰 3V3 才响 → 高触发 → 把这行改成 0
 * ------------------------------------------------------------ */
#define BUZZER_ACTIVE_LOW   1

void buzzer_init(void)
{
    /* ① 开 GPIOB 时钟（外设不上电，写它任何寄存器都无效）*/
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

    /* ② PB0 配成「通用推挽输出 2MHz」
     *    PB0 在低 8 位 → 用 GPIOB_CRL，占最低那 4 位 [3:0]
     *    通用推挽 → CNF=00、MODE=10 → 0b0010 = 0x2
     *    2 MHz 驱动一个高阻输入脚绰绰有余，速度高只会增加 EMI */
    GPIOB->CRL &= ~(0xFUL << 0);
    GPIOB->CRL |=  (0x2UL << 0);

    /* ③ 上电先静音，避免一通电就吵 */
    buzzer_off();
}

void buzzer_on(void)
{
#if BUZZER_ACTIVE_LOW
    GPIOB->BRR  = (1UL << BUZZER_PIN);   /* 输出低 = 触发发声 */
#else
    GPIOB->BSRR = (1UL << BUZZER_PIN);   /* 输出高 = 触发发声 */
#endif
}

void buzzer_off(void)
{
#if BUZZER_ACTIVE_LOW
    GPIOB->BSRR = (1UL << BUZZER_PIN);
#else
    GPIOB->BRR  = (1UL << BUZZER_PIN);
#endif
}

/* ============================================================
 * 带回差的报警判定
 * ============================================================ */
uint8_t buzzer_alarm_update(uint32_t humi_centi, volatile uint8_t *state)
{
    uint8_t st = (state != 0) ? *state : 0U;

    if (humi_centi >= ALARM_HUMI_ON)
    {
        st = 1U;                       /* 越过上限 → 进入报警态 */
    }
    else if (humi_centi <= ALARM_HUMI_OFF)
    {
        st = 0U;                       /* 跌破下限 → 退出报警态 */
    }
    /* 中间那一段（62% ~ 65%）什么也不做 —— 保持原状态，
     * 这就是回差。少了这个 else-if 分支，阈值附近就会抖。 */

    if (state != 0) { *state = st; }

    if (st != 0U) { buzzer_on(); } else { buzzer_off(); }

    return st;
}
