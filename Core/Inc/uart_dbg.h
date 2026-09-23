/* ============================================================
 * uart_dbg.h —— 调试串口 USART1（PA9=TX / PA10=RX，115200-8-N-1）
 *
 * 【本版新增 RX：为什么需要它】
 *   老版本只发不收（PA10 不接）。那样 PC 端只能被动看打印，
 *   没法让板子"按需把 Flash 里的记录吐出来"。
 *   而 W4 的验收要求是【PC 端逐字节校验双 CRC】，
 *   必须能主动发起回读 —— 所以这里补上接收通道。
 *
 * 【为什么在这里放一个环形缓冲】
 *   中断里只做"把字节塞进缓冲"这一件事（几微秒），
 *   真正的命令解析放在通信任务里做（可以慢慢来、可以阻塞）。
 *   这就是"中断只做搬运、任务负责处理"的标准分工 ——
 *   也是简历上"环形缓冲"这个词的真实出处。
 *
 * 【注意：本文件不做任何互斥】
 *   栈溢出钩子（vApplicationStackOverflowHook）里会调用本文件的打印函数，
 *   而那个钩子跑在异常上下文，【绝对不能】再碰 FreeRTOS 的互斥锁
 *   （会二次破坏内核状态）。所以本文件保持"裸"的，
 *   需要多任务安全打印时请用 log.h 里的包装。
 * ============================================================ */
#ifndef UART_DBG_H
#define UART_DBG_H

#include <stdint.h>
#include "board.h"

/* ---- 调试串口所在总线的频率（BRR 由它自动算出）----
 * USART1 挂 APB2 = 72 MHz。
 * 这个宏存在的意义就是防止"时钟提上去了却忘了改 BRR，串口全是乱码"。
 * BRR = 72e6 / 115200 = 625 = 0x271 */
#ifndef UART_DBG_PCLK2_HZ
#define UART_DBG_PCLK2_HZ   BOARD_PCLK2_HZ
#endif

#ifndef UART_DBG_BAUD
#define UART_DBG_BAUD       BOARD_UART_BAUD
#endif

#define UART_DBG_BRR_VAL    ((UART_DBG_PCLK2_HZ + (UART_DBG_BAUD / 2UL)) / UART_DBG_BAUD)

/* ---------- 初始化 ---------- */
void uart_dbg_init(void);

/* ---------- 发送（阻塞式，逐字节等 TXE）---------- */
void uart_dbg_byte(uint8_t b);
void uart_dbg_str(const char *s);
void uart_dbg_u32(uint32_t v);
void uart_dbg_i32(int32_t v);
void uart_dbg_hex8(uint8_t b);
void uart_dbg_nl(void);

/* 打印 32 位十六进制（固定 8 位，用于 dump 记录字节流）*/
void uart_dbg_hex32(uint32_t v);

/* 打印一个字节数组为连续 HEX 串（PC 端解析用）*/
void uart_dbg_hex_buf(const uint8_t *buf, uint32_t len);

/* ---------- 接收（中断写入环形缓冲，任务侧取用）---------- */

/* 返回缓冲里还有多少字节可读 */
uint32_t uart_dbg_rx_available(void);

/* 取一个字节。返回 1 = 取到；0 = 缓冲空 */
uint8_t uart_dbg_rx_pop(uint8_t *out);

/* 丢弃所有未读字节 */
void uart_dbg_rx_flush(void);

/* 诊断：因缓冲满而被丢弃的字节总数。
 * 有统计才叫工程实现 —— 丢数据不可怕，丢了不知道才可怕。 */
uint32_t uart_dbg_rx_dropped(void);

#endif /* UART_DBG_H */
