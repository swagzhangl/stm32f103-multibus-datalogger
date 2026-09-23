/* ============================================================
 * w25q64.c —— W25Q64 SPI NOR Flash 驱动实现
 * ============================================================ */
#include "w25q64.h"
#include "spi_soft.h"
#include "delay_us.h"

/* ---------- 指令码（W25Q64 数据手册 8.1 节指令表）---------- */
#define CMD_WRITE_ENABLE  0x06U   /* 写使能：置 WEL 位 */
#define CMD_READ_STATUS1  0x05U   /* 读状态寄存器 1 */
#define CMD_READ_DATA     0x03U   /* 读数据 */
#define CMD_PAGE_PROGRAM  0x02U   /* 页编程：一次最多 256 字节 */
#define CMD_SECTOR_ERASE  0x20U   /* 扇区擦除：一次 4 KB */
#define CMD_JEDEC_ID      0x9FU   /* 读 JEDEC ID */

/* 状态寄存器 1 的位 */
#define SR1_BUSY   0x01U          /* 1 = 芯片正在内部操作 */
#define SR1_WEL    0x02U          /* 1 = 写使能已锁存 */

/* ---------- 超时上限 ----------
 * 数据手册给的典型/最坏值：
 *   页编程   典型 0.7 ms，最坏 5 ms
 *   扇区擦除 典型 45 ms， 最坏 400 ms
 * 取最坏值的 2.5 倍作为超时，既不会误杀正常操作，也不会真卡死。 */
#define TMO_PAGE_PROGRAM_US   12000UL
#define TMO_SECTOR_ERASE_US   1000000UL

/* ============================================================
 * 内部：轮询 BUSY 位，等内部操作完成
 *
 * ★ CS 全程保持低、只发一次 0x05，之后连续读状态字节 ——
 *   这是数据手册推荐的做法，比"每次重发 0x05"快得多。
 *
 * ★ 必须带超时。嵌入式铁律："永不无限阻塞"。
 *   万一器件异常，卡死在这里会让整个系统失联 ——
 *   而那个"系统失联"的现场，恰恰会让你完全看不到
 *   "其实是 Flash 没响应"这个真实原因。
 * ============================================================ */
static uint8_t wait_busy(uint32_t timeout_us)
{
    uint32_t t0   = delay_now_us();
    uint32_t spin = 0U;
    uint8_t  sr   = 0U;
    uint8_t  dwt  = delay_is_dwt_ok();

    spi_cs_low();
    (void)spi_soft_transfer(CMD_READ_STATUS1);

    for (;;)
    {
        sr = spi_soft_transfer(0xFFU);
        if ((sr & SR1_BUSY) == 0U) { break; }

        spin++;
        if (dwt != 0U)
        {
            /* ★ 必须用 delay_elapsed_us() 而不是 (delay_now_us() - t0)：
             *   delay_now_us() 每 59.65 s 回绕一次（不是 2^32 µs），
             *   跨回绕点直接相减会得到约 4.2e9 的假值 → 假超时。
             *   实测后果：页编程被误判失败 → 写指针不推进 →
             *   下次往同一槽位重写 → 按位与 → 半写记录。详见 delay_us.h。 */
            if (delay_elapsed_us(t0) >= timeout_us) { break; }
        }
        else
        {
            /* DWT 不可用的退化路径：按循环次数估（每圈约 3 µs）*/
            if (spin >= (timeout_us / 3UL)) { break; }
        }
    }

    spi_cs_high();

    return (uint8_t)(((sr & SR1_BUSY) != 0U) ? 1U : 0U);   /* 1 = 超时未完成 */
}

/* ============================================================
 * 读 JEDEC ID
 *
 * SPI **没有 ACK 机制**，主机发完不知道从机收没收到。
 * 唯一能自检的办法就是"读一个从机出厂就固定好的值"：
 * 对上了就说明接线通、时钟正常、模式（CPOL/CPHA）正确、供电正常。
 * ⇒ 这就是为什么每次 bring-up 都先干这件事，而不是急着写数据。
 * ============================================================ */
void w25q64_read_id(uint8_t id[3])
{
    spi_cs_low();
    (void)spi_soft_transfer(CMD_JEDEC_ID);
    id[0] = spi_soft_transfer(0xFFU);   /* 厂商 ID */
    id[1] = spi_soft_transfer(0xFFU);   /* 存储类型 */
    id[2] = spi_soft_transfer(0xFFU);   /* 容量代码 */
    spi_cs_high();
}

uint8_t w25q64_is_id_ok(const uint8_t id[3])
{
    return (uint8_t)(((id[0] == W25Q64_ID_MFR) &&
                      (id[1] == W25Q64_ID_TYPE) &&
                      (id[2] == W25Q64_ID_CAP)) ? 1U : 0U);
}

uint8_t w25q64_read_status(void)
{
    uint8_t sr;

    spi_cs_low();
    (void)spi_soft_transfer(CMD_READ_STATUS1);
    sr = spi_soft_transfer(0xFFU);
    spi_cs_high();

    return sr;
}

uint8_t w25q64_is_busy(void)
{
    return (uint8_t)(((w25q64_read_status() & SR1_BUSY) != 0U) ? 1U : 0U);
}

/* ============================================================
 * 写使能
 *
 * ★ 每次"写"或"擦"之前都必须先发 0x06，否则芯片会【静默忽略】该指令：
 *   不报错、不烧毁、就是不干活。
 *   这类"syntax 正确、语义无效"的行为是 Flash 调试里最常见的坑，
 *   所以本函数回读 WEL 位确认 —— 这是"写使能真的生效了"的唯一证据。
 * ============================================================ */
uint8_t w25q64_write_enable(void)
{
    spi_cs_low();
    (void)spi_soft_transfer(CMD_WRITE_ENABLE);
    spi_cs_high();

    return (uint8_t)(((w25q64_read_status() & SR1_WEL) != 0U) ? 0U : 1U);
}

uint8_t w25q64_sector_erase(uint32_t addr)
{
    if (w25q64_write_enable() != 0U) { return 1U; }

    spi_cs_low();
    (void)spi_soft_transfer(CMD_SECTOR_ERASE);
    (void)spi_soft_transfer((uint8_t)(addr >> 16));   /* 24 位地址，高字节先发 */
    (void)spi_soft_transfer((uint8_t)(addr >> 8));
    (void)spi_soft_transfer((uint8_t)(addr));
    spi_cs_high();

    return (uint8_t)((wait_busy(TMO_SECTOR_ERASE_US) != 0U) ? 2U : 0U);
}

uint8_t w25q64_page_program(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    /* 前置检查一：长度必须 ≤ 256 */
    if ((len == 0U) || (len > (uint16_t)W25Q64_PAGE_SIZE)) { return 1U; }

    /* 前置检查二：不能跨页。
     * 跨页时多出来的字节会"回卷"到本页开头，把前面刚写的数据覆盖掉 ——
     * 而且是静默覆盖，读回来才发现不对。 */
    if (((addr & (W25Q64_PAGE_SIZE - 1UL)) + (uint32_t)len) > W25Q64_PAGE_SIZE) { return 2U; }

    if (w25q64_write_enable() != 0U) { return 3U; }

    spi_cs_low();
    (void)spi_soft_transfer(CMD_PAGE_PROGRAM);
    (void)spi_soft_transfer((uint8_t)(addr >> 16));
    (void)spi_soft_transfer((uint8_t)(addr >> 8));
    (void)spi_soft_transfer((uint8_t)(addr));

    for (i = 0U; i < len; i++)
    {
        (void)spi_soft_transfer(buf[i]);
    }
    spi_cs_high();

    return (uint8_t)((wait_busy(TMO_PAGE_PROGRAM_US) != 0U) ? 4U : 0U);
}

void w25q64_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;

    spi_cs_low();
    (void)spi_soft_transfer(CMD_READ_DATA);
    (void)spi_soft_transfer((uint8_t)(addr >> 16));
    (void)spi_soft_transfer((uint8_t)(addr >> 8));
    (void)spi_soft_transfer((uint8_t)(addr));

    for (i = 0U; i < len; i++)
    {
        /* 读的时候主机发什么无所谓，习惯发 0xFF */
        buf[i] = spi_soft_transfer(0xFFU);
    }

    spi_cs_high();
}
