/* ============================================================
 * w25q64.h —— W25Q64 SPI NOR Flash 驱动（8 MB / 64 Mbit）
 *
 * 【NOR Flash 的两条铁律】（面试必问，也是本项目 W4 的验收核心）
 *   ① 位只能 1→0。写操作只能把 1 变成 0，不能把 0 变回 1。
 *      ⇒ 写入之前必须先【擦除整个扇区】（擦除会把整扇区变回 0xFF）。
 *        忘了擦的后果不是报错，而是"新数据与旧数据按位相与"，
 *        存进去的数据完全不对，而且没有任何错误码提示你 ——
 *        这是 Flash 调试里最阴的一类问题。
 *   ② 擦/写指令发出后，芯片进入内部忙状态（几十 µs 到几百 ms），
 *      期间只响应"读状态寄存器"。
 *      ⇒ 必须轮询状态寄存器 1 的 BUSY 位（bit0）等待完成，
 *        不能立刻发下一条指令（会被静默忽略）。
 *
 * 【容量与边界】
 *   总容量 8 MB，组织成：256 B/页 × 16 页 = 4096 B/扇区
 *   页编程不能跨页 —— 跨页时多出来的字节会"回卷"到本页开头覆盖前面的数据
 * ============================================================ */
#ifndef W25Q64_H
#define W25Q64_H

#include <stdint.h>

#define W25Q64_TOTAL_SIZE   (8UL * 1024UL * 1024UL)   /* 8 MB = 64 Mbit */
#define W25Q64_PAGE_SIZE    256UL                     /* 页：编程的最小单位 */
#define W25Q64_SECTOR_SIZE  4096UL                    /* 扇区：擦除的最小单位 */

/* JEDEC ID：厂商 0xEF(Winbond) + 类型 0x40 + 容量 0x17(=2^23 = 8 MB) */
#define W25Q64_ID_MFR       0xEFU
#define W25Q64_ID_TYPE      0x40U
#define W25Q64_ID_CAP       0x17U

/* 读 JEDEC ID（命令 0x9F）。SPI 没有 ACK，这是唯一的连通性自检手段 */
void w25q64_read_id(uint8_t id[3]);
uint8_t w25q64_is_id_ok(const uint8_t id[3]);

/* 读状态寄存器 1（命令 0x05）。bit0 = BUSY，bit1 = WEL */
uint8_t w25q64_read_status(void);
uint8_t w25q64_is_busy(void);

/* 写使能（命令 0x06）。返回 0 = WEL 已置位，1 = 失败。
 * ★ 每一条写/擦指令之前都必须重新发一次 —— WEL 在操作开始后会自动清零。 */
uint8_t w25q64_write_enable(void);

/* 扇区擦除（命令 0x20，4 KB）。返回 0 = 成功，1 = 写使能失败，2 = 超时 */
uint8_t w25q64_sector_erase(uint32_t addr);

/* 页编程（命令 0x02，≤256 B 且不得跨页）。
 * 返回 0 = 成功，1 = 长度非法，2 = 跨页，3 = 写使能失败，4 = 超时 */
uint8_t w25q64_page_program(uint32_t addr, const uint8_t *buf, uint16_t len);

/* 读数据（命令 0x03）。可任意地址、任意长度，跨页跨扇区都没问题 */
void w25q64_read(uint32_t addr, uint8_t *buf, uint32_t len);

#endif /* W25Q64_H */
