/* ============================================================
 * spi_soft.c —— bit-bang SPI 主机实现（四种模式）
 *
 * 【和 I2C 的根本区别】
 *   I2C：两根线（SCL/SDA）双向复用，有 START/STOP/ACK，从机有地址
 *   SPI：四根线各管一件事（CS/SCK/MOSI/MISO），没有地址、没有 ACK，
 *        CS 拉低就是"我要跟你说话"，CS 拉高就是"说完了"
 *   ⇒ SPI 主机【无法知道从机是否收到】。唯一的自检手段是
 *     "读一个出厂就固定好的值"（JEDEC ID），对上了才说明通。
 *
 * 【两个边沿各干什么活】
 *   每个时钟周期有两个边沿：前沿（离开空闲态）和后沿（回到空闲态）。
 *     CPHA=0：数据在【前沿之前】就摆好 —— 于是前沿采样
 *     CPHA=1：数据在【前沿之时】才改变 —— 于是后沿采样
 *   本文件用 sck_edge_idle_to_active / sck_edge_active_to_idle
 *   两个函数把"前沿/后沿"抽象出来，四种模式就只剩下参数差别。
 * ============================================================ */
#include "stm32f1xx.h"
#include "spi_soft.h"
#include "board.h"
#include "delay_us.h"

/* ---------- 引脚位掩码 ---------- */
#define CS_BIT    (1U << SPI_CS_PIN)
#define SCK_BIT   (1U << SPI_SCK_PIN)
#define MISO_BIT  (1U << SPI_MISO_PIN)
#define MOSI_BIT  (1U << SPI_MOSI_PIN)

/* 当前模式（由 spi_soft_set_mode 维护）*/
static uint8_t s_cpol        = 0U;   /* 空闲电平：0 = 低，1 = 高 */
static uint8_t s_cpha        = 0U;   /* 采样沿：0 = 前沿，1 = 后沿 */
static uint8_t s_bit_delay   = 0U;   /* 每位延时（µs），0 = 全速 */

/* ---------- 引脚操作 ----------
 * 一律用 BSRR/BRR 原子写，不用 ODR 的"读-改-写"：
 * ODR 要读出来、改一位、写回去三步，被中断打断就可能丢状态。
 * BSRR/BRR 是"写 1 生效"，一条语句搞定。 */
static void pin_high(uint32_t bit) { GPIOA->BSRR = bit; }
static void pin_low (uint32_t bit) { GPIOA->BRR  = bit; }

static void mosi_set(uint8_t level)
{
    if (level != 0U) { pin_high(MOSI_BIT); } else { pin_low(MOSI_BIT); }
}

static uint8_t miso_get(void)
{
    return ((GPIOA->IDR & MISO_BIT) != 0U) ? 1U : 0U;
}

static void bit_delay(void)
{
    if (s_bit_delay != 0U) { delay_us(s_bit_delay); }
}

/* ---------- 时钟边沿 ---------- */

/* 把 SCK 摆到指定 CPOL 对应的空闲电平 */
static void sck_set_idle(uint8_t cpol)
{
    if (cpol != 0U) { pin_high(SCK_BIT); } else { pin_low(SCK_BIT); }
}

/* 前沿：空闲 → 有效。CPOL=0 时是上升沿，CPOL=1 时是下降沿 */
static void sck_edge_idle_to_active(uint8_t cpol)
{
    if (cpol != 0U) { pin_low(SCK_BIT); } else { pin_high(SCK_BIT); }
}

/* 后沿：有效 → 空闲。方向与前沿相反 */
static void sck_edge_active_to_idle(uint8_t cpol)
{
    if (cpol != 0U) { pin_high(SCK_BIT); } else { pin_low(SCK_BIT); }
}

/* ============================================================
 * 初始化
 * ============================================================ */
void spi_soft_init(void)
{
    /* ① 开 GPIOA 时钟 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;

    /* ② 配 PA4~PA7。GPIOA_CRL 每 4 位管一根脚，PA4~PA7 正好占高 16 位：
     *      PA4 → [19:16]   PA5 → [23:20]   PA6 → [27:24]   PA7 → [31:28]
     *    推挽输出 50MHz → CNF=00 MODE=11 → 0b0011 = 0x3
     *    浮空输入      → CNF=01 MODE=00 → 0b0100 = 0x4
     *    （MISO 由从机驱动，主机只能当输入；从机不驱动时是高阻，
     *      所以"浮空输入"最合适，不用上拉） */
    GPIOA->CRL &= ~(0xFFFFUL << 16);
    GPIOA->CRL |=  (0x3UL << 16)     /* PA4 CS   ：推挽输出 */
               |   (0x3UL << 20)     /* PA5 SCK  ：推挽输出 */
               |   (0x4UL << 24)     /* PA6 MISO ：浮空输入 */
               |   (0x3UL << 28);    /* PA7 MOSI ：推挽输出 */

    spi_soft_set_mode(SPI_MODE0);
    spi_cs_high();      /* ★ 空闲时片选必须拉高，否则从机会误以为在通信 */
}

void spi_soft_set_mode(uint8_t mode)
{
    s_cpol = (uint8_t)((mode >> 1) & 1U);
    s_cpha = (uint8_t)(mode & 1U);

    /* ★ 切模式后必须立刻把 SCK 摆到新模式的空闲电平，
     *   否则 SCK 停在旧电平，从机看到的第一个边沿方向就是错的。 */
    sck_set_idle(s_cpol);
}

uint8_t spi_soft_get_mode(void)
{
    return (uint8_t)((uint8_t)(s_cpol << 1) | s_cpha);
}

void spi_soft_set_bit_delay_us(uint8_t us)
{
    s_bit_delay = us;
}

void spi_cs_low(void)  { pin_low(CS_BIT);  }
void spi_cs_high(void) { pin_high(CS_BIT); }

void spi_soft_idle_us(uint32_t us)
{
    delay_us(us);
}

/* ============================================================
 * 收发一个字节（按当前模式）
 * ============================================================ */
uint8_t spi_soft_transfer(uint8_t tx)
{
    uint8_t mode = spi_soft_get_mode();
    return spi_soft_transfer_ex(tx, mode, mode);
}

/* ============================================================
 * 收发一个字节（可分别指定发送侧与接收侧的模式）
 *
 * 【什么时候需要拆开发送和接收的模式？】
 *   正常通信中主从模式必然一致，用不到这个函数。
 *   它存在的唯一目的，是让"模式配对错误"这件事【可以被看见】。
 *
 * 【为什么单纯的杜邦线回环看不到 CPHA 配错】
 *   把 MOSI 和 MISO 用一根线短接，主机既发又收、且用的是同一个模式，
 *   那么"什么时候把数据摆上线"和"什么时候去读线"是同一个人控制的，
 *   两者永远自洽 —— 四种模式都会读回原值。
 *   所以「回环自测」能证明的是【位引擎正确】（移位方向、MISO 通路、
 *   边沿计数都对），它【证明不了模式配对对不对】。
 *   要证明模式配对，得靠真实从机（W25Q64）或逻辑分析仪波形。
 *
 * 【本函数的教学价值】
 *   指定"发送方按 CPHA=1（前沿换数据）、接收方按 CPHA=0（前沿采样）"，
 *   接收方的采样点就正好压在数据跳变的那一刻 —— 建立时间不满足，
 *   于是读到的是相邻位，整体错位半拍。这正是现实中
 *   "主从模式配错 → 收到 0xA5 变成 0x52"的成因。
 * ============================================================ */
uint8_t spi_soft_transfer_ex(uint8_t tx, uint8_t tx_mode, uint8_t rx_mode)
{
    uint8_t tcpol = (uint8_t)((tx_mode >> 1) & 1U);
    uint8_t tcpha = (uint8_t)(tx_mode & 1U);
    uint8_t rcpol = (uint8_t)((rx_mode >> 1) & 1U);
    uint8_t rcpha = (uint8_t)(rx_mode & 1U);
    uint8_t sample_at_edge1;
    uint8_t rx = 0U;
    int8_t  i;

    /* 接收方在哪一个边沿采样，换算到本函数产生的两个边沿上：
     *   若收发 CPOL 相同 → 本函数的前沿就是接收方的"第一个边沿"
     *   若收发 CPOL 不同 → 本函数的前沿对接收方来说是"第二个边沿"
     *   CPHA=0 采第一个边沿，CPHA=1 采第二个边沿 */
    sample_at_edge1 = (uint8_t)((((tcpol == rcpol) ? 1U : 0U) == ((rcpha == 0U) ? 1U : 0U)) ? 1U : 0U);

    /* 时钟先摆到发送方的空闲电平 */
    sck_set_idle(tcpol);
    bit_delay();

    for (i = 7; i >= 0; i--)
    {
        uint8_t bit = (uint8_t)((tx >> i) & 1U);

        /* ---- 数据准备 ----
         * CPHA=0：数据必须在【前沿之前】就稳定 */
        if (tcpha == 0U) { mosi_set(bit); }
        bit_delay();

        /* ---- 前沿：空闲 → 有效 ---- */
        sck_edge_idle_to_active(tcpol);
        bit_delay();

        if (sample_at_edge1 != 0U)
        {
            rx = (uint8_t)((uint8_t)(rx << 1) | miso_get());
        }

        /* CPHA=1：数据在【前沿之时】改变。
         * 注意这一句排在采样之后 —— 这正是"采样点与数据变化点重合时
         * 会读到相邻位"的根源（建立时间不足），也就是移位半拍的成因。 */
        if (tcpha == 1U) { mosi_set(bit); }
        bit_delay();

        /* ---- 后沿：有效 → 空闲 ---- */
        sck_edge_active_to_idle(tcpol);
        bit_delay();

        if (sample_at_edge1 == 0U)
        {
            rx = (uint8_t)((uint8_t)(rx << 1) | miso_get());
        }
    }

    /* 收尾把 SCK 放回空闲电平，方便逻辑分析仪看到清晰的静态电平 */
    sck_set_idle(tcpol);
    bit_delay();

    return rx;
}
