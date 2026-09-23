/* ============================================================
 * uart_dbg.c —— USART1 调试串口实现（寄存器级）
 *
 * 【接线】
 *   PA9  (板子丝印 A9) → CH340 的 RXD     发送
 *   PA10 (板子丝印 A10)→ CH340 的 TXD     接收（本版新增）
 *   两边必须共地。
 *
 * 【本文件包含三块】
 *   ① USART1 初始化（含 RX）
 *   ② 阻塞式发送 + 各种格式化输出
 *   ③ RXNE 中断 + 环形缓冲区（中断只搬运，任务去消费）
 * ============================================================ */
#include "stm32f1xx.h"
#include "uart_dbg.h"

/* ============================================================
 * ① 初始化
 * ============================================================ */
void uart_dbg_init(void)
{
    /* ---- 开时钟：IOPAEN(GPIOA, bit2) + USART1EN(bit14) ---- */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;

    /* ---- PA9 = 复用推挽输出 50 MHz ----
     * GPIOA_CRH 每 4 位管一根脚：PA8=[3:0] PA9=[7:4] PA10=[11:8]
     * 复用推挽 50MHz → CNF=10 MODE=11 → 0b1011 = 0xB */
    GPIOA->CRH &= ~(0xFU << 4);
    GPIOA->CRH |=  (0xBU << 4);

    /* ---- PA10 = 上拉输入 ----
     * 输入上拉 → CNF=10 MODE=00 → 0b1000 = 0x8
     * ★ 用上拉而不是浮空：串口线没插的时候，浮空输入会收到一堆噪声，
     *   软件上就表现为"莫名其妙的乱码命令"。上拉把它钉在高电平。 */
    GPIOA->CRH &= ~(0xFU << 8);
    GPIOA->CRH |=  (0x8U << 8);

    /* ---- 波特率：625 = 0x271（见 board.h 的推导）---- */
    USART1->BRR = UART_DBG_BRR_VAL;

    /* ---- 控制寄存器：一次把要用的位全开 ----
     *   UE      (bit13) 串口总使能
     *   TE      (bit3)  发送使能
     *   RE      (bit2)  接收使能
     *   RXNEIE  (bit5)  接收数据寄存器非空中断
     *
     * ★ 顺序有讲究：UE 放最前面。BRR 和 CR1 的其它位都可以在 UE=0 时写，
     *   但串口的实际行为由 UE 使能后才生效。 */
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

    /* ---- 中断优先级 ----
     * 这个中断里不调用任何 FreeRTOS API（只往环形缓冲塞字节），
     * 所以不受 configMAX_SYSCALL_INTERRUPT_PRIORITY 约束。
     * 给它 6 这个偏高的优先级（数值越小越高），保证高速收数据时不丢字节。 */
    NVIC_SetPriority(USART1_IRQn, 6U);
    NVIC_EnableIRQ(USART1_IRQn);
}

/* ============================================================
 * ② 发送
 * ============================================================ */

void uart_dbg_byte(uint8_t b)
{
    /* 等 TXE（发送数据寄存器空）：说明上一个字节已经被搬进移位寄存器，
     * DR 可以接收新字节了。这是轮询式发送的标准写法。 */
    while ((USART1->SR & USART_SR_TXE) == 0U) { }
    USART1->DR = (uint32_t)b;
}

void uart_dbg_str(const char *s)
{
    while (*s != '\0')
    {
        uart_dbg_byte((uint8_t)(*s));
        s++;
    }
}

void uart_dbg_nl(void)
{
    uart_dbg_byte('\r');
    uart_dbg_byte('\n');
}

void uart_dbg_u32(uint32_t v)
{
    char    tmp[11];
    int     i = 0;

    if (v == 0U) { uart_dbg_byte('0'); return; }

    while ((v > 0U) && (i < 10))
    {
        tmp[i] = (char)('0' + (v % 10U));
        v /= 10U;
        i++;
    }
    while (i > 0) { i--; uart_dbg_byte((uint8_t)tmp[i]); }
}

void uart_dbg_i32(int32_t v)
{
    if (v < 0)
    {
        uart_dbg_byte('-');
        /* ★ 这里不能写 v = -v：当 v = INT32_MIN 时 -v 溢出。
         *   用无符号取负（0 - (uint32_t)v）是定义良好的行为。 */
        uart_dbg_u32(0U - (uint32_t)v);
        return;
    }
    uart_dbg_u32((uint32_t)v);
}

static const char HEX[] = "0123456789ABCDEF";

void uart_dbg_hex8(uint8_t b)
{
    uart_dbg_byte((uint8_t)HEX[(b >> 4) & 0x0FU]);
    uart_dbg_byte((uint8_t)HEX[b & 0x0FU]);
}

void uart_dbg_hex32(uint32_t v)
{
    int shift;
    for (shift = 28; shift >= 0; shift -= 4)
    {
        uart_dbg_byte((uint8_t)HEX[(v >> shift) & 0x0FU]);
    }
}

void uart_dbg_hex_buf(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    for (i = 0U; i < len; i++)
    {
        uart_dbg_hex8(buf[i]);
    }
}

/* ============================================================
 * ③ 接收：RXNE 中断 + 环形缓冲区
 *
 * 单生产者（中断）/ 单消费者（任务）模型，所以不需要临界区：
 *   · head 只被中断改写，任务只读
 *   · tail 只被任务改写，中断只读
 * 这是无锁环形缓冲能成立的唯一前提 —— 一旦有两方都写 head，就必须加锁。
 * ============================================================ */
#define RX_BUF_SIZE     128U
#define RX_BUF_MASK     (RX_BUF_SIZE - 1U)

static volatile uint8_t  s_rx_buf[RX_BUF_SIZE];
static volatile uint32_t s_rx_head = 0U;    /* 中断侧写入位置 */
static volatile uint32_t s_rx_tail = 0U;    /* 任务侧读取位置 */
static volatile uint32_t s_rx_drop = 0U;    /* 缓冲满而丢弃的字节数 */

/* 中断里调用：塞一个字节。缓冲满则丢弃【最新】字节并计数。 */
static void rx_push(uint8_t b)
{
    uint32_t next = (s_rx_head + 1U) & RX_BUF_MASK;

    if (next == s_rx_tail)
    {
        /* 满了。这里选择丢"最新"而不是"最旧"：
         * 命令是短报文，丢最新的影响面更小；而丢最旧会让半条命令残留，
         * 反而污染后续解析。无论丢哪边，都必须【计数】。 */
        s_rx_drop++;
        return;
    }

    s_rx_buf[s_rx_head] = b;
    s_rx_head = next;
}

uint32_t uart_dbg_rx_available(void)
{
    return (s_rx_head - s_rx_tail) & RX_BUF_MASK;
}

uint8_t uart_dbg_rx_pop(uint8_t *out)
{
    if (s_rx_head == s_rx_tail) { return 0U; }

    *out = s_rx_buf[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1U) & RX_BUF_MASK;
    return 1U;
}

void uart_dbg_rx_flush(void)
{
    s_rx_tail = s_rx_head;
}

uint32_t uart_dbg_rx_dropped(void)
{
    return s_rx_drop;
}

/* ============================================================
 * USART1 中断服务程序
 *
 * 只做两件事：读走字节、塞进缓冲。解析交给通信任务。
 * 中断里越短越好 —— 每多 1 µs，就是在抢走所有任务的 CPU 时间。
 * ============================================================ */
void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;

    /* RXNE = 收到新字节；ORE = 溢出（上一个还没读走又来了一个）
     * ★ 两者都必须靠"读 DR"来清除。
     *   只清 RXNE 不处理 ORE，会留下一个粘滞的溢出标志，
     *   之后串口就再也进不了中断了 —— 这是很隐蔽的一个坑。 */
    if ((sr & (USART_SR_RXNE | USART_SR_ORE)) != 0U)
    {
        uint8_t b = (uint8_t)USART1->DR;   /* 读 DR：同时清 RXNE 与 ORE */

        if ((sr & USART_SR_RXNE) != 0U)
        {
            rx_push(b);
        }
    }
}
