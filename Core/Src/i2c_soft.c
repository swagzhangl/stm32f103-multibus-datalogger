/* ============================================================
 * i2c_soft.c —— GPIO 软件模拟（bit-bang）I2C 主机
 *
 * 【为什么用开漏（Open-Drain）—— I2C 的物理基础】
 *   I2C 总线是"线与"结构：任何一方拉低即为低，所有人都不拉才是高
 *   （高电平由外部上拉电阻提供）。
 *   推挽输出会主动输出高电平，当别人正拉低时，两个管子直接对打 ——
 *   轻则通信失败，重则长期短路烧引脚。
 *   ⇒ 开漏模式下引脚只有两种状态："拉低"或"松开"，
 *     松开时它自己不产生任何电平，交给上拉电阻。
 *
 * 【SDA 方向怎么切换】
 *   常规做法是"发的时候配输出、收的时候配输入"，每次都要动 CRL 寄存器。
 *   更优雅的做法：全程保持开漏输出模式，用 ODR 位控制 ——
 *     ODR 写 0 = 拉低；ODR 写 1 = 松开（等于交给从机/上拉）。
 *   读引脚真实电平去 IDR 读（即使 ODR 写了 1，只要别人拉低，IDR 读到还是 0）。
 *   本驱动采用后者，代码短且不会漏配方向。
 *
 * 【延时的标定 —— 本版最重要的改动】
 *   老版本用 `for (i=0;i<5;i++)` 空循环，注释自己写着"HSI 8 MHz 下约 3~4 µs"。
 *   主频提到 72 MHz 后同样的循环快了 9 倍，I2C 会跑到 ~900 kHz，
 *   远超 BME280 的 400 kHz 上限，表现为"8 MHz 下好好的，一提频就不通"。
 *   ⇒ 本版改用 DWT 周期计数器延时（见 delay_us.c），与主频严格绑定，
 *     并可通过 i2c_soft_set_delay_us() 在线降速，便于排查。
 * ============================================================ */
#include "stm32f1xx.h"
#include "i2c_soft.h"
#include "board.h"
#include "delay_us.h"

/* ---------- 引脚位掩码 ---------- */
#define SCL_BIT   (1U << I2C_SCL_PIN)
#define SDA_BIT   (1U << I2C_SDA_PIN)

/* ---------- 默认半周期延时 ----------
 * I2C 100 kHz → 周期 10 µs → 半周期 5 µs。
 * 每个边沿 = 1 次 delay_us + 几次寄存器读写（约 0.5 µs），
 * 所以取 4 µs 得到大约 100 kHz，落在标准模式（100 kHz）附近，
 * 又给"接线较长、上升沿变缓"留了余量。 */
#define I2C_DELAY_US_DEFAULT   4U

static uint8_t  s_delay_us  = I2C_DELAY_US_DEFAULT;
static uint32_t s_nack_cnt  = 0U;
static uint32_t s_recov_cnt = 0U;

/* ============================================================
 * 底层原语
 * ============================================================ */
static void i2c_delay(void)
{
    delay_us(s_delay_us);
}

static void scl_low(void)  { GPIOB->BRR  = SCL_BIT; i2c_delay(); }
static void scl_high(void) { GPIOB->BSRR = SCL_BIT; i2c_delay(); }

/* SDA 拉低 / 松开（松开 = 写 1，靠上拉回高，控制权交给从机） */
static void sda_low(void)  { GPIOB->BRR  = SDA_BIT; i2c_delay(); }
static void sda_high(void) { GPIOB->BSRR = SDA_BIT; i2c_delay(); }

/* 读 SDA 真实电平：开漏下即使 ODR 写 1，若从机拉低，IDR 读回仍是 0 */
static uint8_t sda_read(void) { return ((GPIOB->IDR & SDA_BIT) != 0U) ? 1U : 0U; }
static uint8_t scl_read(void) { return ((GPIOB->IDR & SCL_BIT) != 0U) ? 1U : 0U; }

/* ---------- 起始 / 停止条件 ----------
 * I2C 只允许在【SCL 为高】的时候翻转 SDA，这两个"非法电平跳变"
 * 正好被定义成 START 和 STOP，是个很巧妙的设计。 */

static void i2c_start(void)
{
    sda_high();          /* 确保 SDA 先释放为高（否则 START 无法成立）*/
    scl_high();          /* SCL 拉高 */
    sda_low();           /* ★ SCL 高时拉低 SDA = START */
    scl_low();           /* 拉低 SCL，准备发第一位 */
}

static void i2c_stop(void)
{
    sda_low();           /* 先保证 SDA 为低 */
    scl_high();          /* SCL 拉高 */
    sda_high();          /* ★ SCL 高时释放 SDA = STOP */
}

/* ---------- 字节收发 ---------- */

/* 发一个字节（MSB 先行），返回从机应答：0 = ACK，1 = NACK */
static uint8_t i2c_write_byte(uint8_t b)
{
    uint8_t bit;
    uint8_t nack;

    for (bit = 0U; bit < 8U; bit++)
    {
        if ((b & 0x80U) != 0U) { sda_high(); }   /* 最高位 1 → 松开 */
        else                   { sda_low();  }   /* 最高位 0 → 拉低 */

        scl_high();                  /* SCL 上升沿：从机在此采样数据 */
        scl_low();                   /* SCL 拉低：本位结束 */
        b = (uint8_t)(b << 1);       /* 左移，准备下一位 */
    }

    /* 第 9 个时钟：读 ACK。此刻主机必须松开 SDA，让从机拉低表示应答 */
    sda_high();
    scl_high();
    nack = sda_read();     /* 0 = 从机拉低 = ACK；1 = 没人应答 = NACK */
    scl_low();

    return nack;
}

/* 收一个字节。参数 ack：0 = 还要继续读（主机回 ACK），1 = 最后一个（回 NACK）*/
static uint8_t i2c_read_byte(uint8_t ack)
{
    uint8_t b   = 0U;
    uint8_t bit;

    sda_high();     /* 主机松开 SDA，把总线交给从机驱动 */

    for (bit = 0U; bit < 8U; bit++)
    {
        b = (uint8_t)(b << 1);
        scl_high();                          /* SCL 高电平期间数据有效，读 */
        if (sda_read() != 0U) { b |= 1U; }
        scl_low();                           /* SCL 低：从机准备下一位 */
    }

    /* 第 9 个时钟：主机回 ACK / NACK */
    if (ack != 0U) { sda_high(); }    /* 最后一个字节：NACK，告诉从机"够了" */
    else           { sda_low();  }    /* 还要继续收：ACK，让从机继续发 */

    scl_high();
    scl_low();
    sda_high();                       /* 收尾松开 SDA */

    return b;
}

/* ============================================================
 * 对外接口
 * ============================================================ */
void i2c_soft_init(void)
{
    /* ① 开 GPIOB 时钟 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

    /* ② PB6/PB7 配成开漏输出 2 MHz
     *    CRL 里 PB6 是第 6 个四位组（位 [27:24]），PB7 是第 7 个（位 [31:28]）
     *    开漏输出 → CNF=01、MODE=10 → 0b0110 = 0x6
     *    （MODE=10 是输出 2 MHz。这里刻意不用 50 MHz：
     *      2 MHz 的翻转速率较低，边沿更缓、振铃更小，对 I2C 更友好）
     *    先清后置，不动其他引脚 */
    GPIOB->CRL &= ~(0xFFU << 24);
    GPIOB->CRL |=  (0x66U << 24);

    /* ③ 释放总线：SCL/SDA 都写 1（松开，靠上拉回高）= 总线空闲态 */
    GPIOB->BSRR = SCL_BIT | SDA_BIT;
}

void i2c_soft_set_delay_us(uint8_t us)
{
    s_delay_us = (us == 0U) ? 1U : us;
}

uint8_t i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t i;

    /* 阶段 1：START + 设备地址（写方向：addr<<1 | 0）*/
    i2c_start();
    if (i2c_write_byte((uint8_t)((uint8_t)(addr7 << 1) | 0x00U)) != 0U)
    {
        i2c_stop(); s_nack_cnt++; return 1U;
    }

    /* 阶段 2：要读哪个寄存器 */
    if (i2c_write_byte(reg) != 0U)
    {
        i2c_stop(); s_nack_cnt++; return 2U;
    }

    /* 阶段 3：Repeated START + 设备地址（读方向：addr<<1 | 1）
     * ★ 中间【不产生 STOP】。这就叫"重复起始"。
     *   如果中间插了 STOP，别的总线主机可能抢占总线，
     *   导致"写寄存器地址"和"读数据"两段落在不同的事务里 ——
     *   这就是复合事务必须用 Repeated Start 的原因。 */
    i2c_start();
    if (i2c_write_byte((uint8_t)((uint8_t)(addr7 << 1) | 0x01U)) != 0U)
    {
        i2c_stop(); s_nack_cnt++; return 3U;
    }

    /* 阶段 4：连续读 len 个字节；前 len-1 个回 ACK，最后一个回 NACK */
    for (i = 0U; i < len; i++)
    {
        buf[i] = i2c_read_byte((i == (uint8_t)(len - 1U)) ? 1U : 0U);
    }

    i2c_stop();
    return 0U;
}

uint8_t i2c_write_reg(uint8_t addr7, uint8_t reg, const uint8_t *buf, uint8_t len)
{
    uint8_t i;

    i2c_start();
    if (i2c_write_byte((uint8_t)((uint8_t)(addr7 << 1) | 0x00U)) != 0U)
    {
        i2c_stop(); s_nack_cnt++; return 1U;
    }
    if (i2c_write_byte(reg) != 0U)
    {
        i2c_stop(); s_nack_cnt++; return 2U;
    }

    for (i = 0U; i < len; i++)
    {
        if (i2c_write_byte(buf[i]) != 0U)
        {
            i2c_stop(); s_nack_cnt++; return 3U;
        }
    }

    i2c_stop();
    return 0U;
}

/* ============================================================
 * 总线复位
 *
 * 【什么时候需要】
 *   主机在从机正在输出某一位时被复位/中断打断，从机会一直把 SDA
 *   拉低等下一个时钟 —— 从从机的视角看它是"卡在事务中间"的。
 *   此时主机再发 START 也没用（START 要求 SDA 从高变低，
 *   而 SDA 一直是低），表现为"程序看着没错，就是一条都不通"。
 *
 * 【怎么救】
 *   手动发最多 9 个 SCL 脉冲，让从机把剩下的位全部吐完，
 *   它自然就会释放 SDA；然后补一个 STOP 让它回到空闲。
 * ============================================================ */
uint8_t i2c_soft_bus_recover(void)
{
    uint8_t i;

    s_recov_cnt++;

    /* SCL/SDA 都先松开 */
    GPIOB->BSRR = SCL_BIT | SDA_BIT;
    i2c_delay();

    /* 如果 SDA 已经被拉低（从机卡住），打 9 个时钟 */
    if (sda_read() == 0U)
    {
        for (i = 0U; i < 9U; i++)
        {
            scl_low();
            scl_high();
            if (sda_read() != 0U) { break; }   /* 从机松手了 */
        }
        /* 补一个 STOP：SCL 高时 SDA 由低变高 */
        sda_low();
        scl_high();
        sda_high();
    }

    /* 判定：SCL 与 SDA 都是高才算真正回到空闲 */
    return ((sda_read() != 0U) && (scl_read() != 0U)) ? 1U : 0U;
}

uint32_t i2c_soft_nack_count(void)  { return s_nack_cnt;  }
uint32_t i2c_soft_recover_count(void){ return s_recov_cnt; }
