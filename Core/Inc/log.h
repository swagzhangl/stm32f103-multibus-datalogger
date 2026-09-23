/* ============================================================
 * log.h —— 多任务安全的串口打印
 *
 * 【为什么不能直接用 uart_dbg_*】
 *   uart_dbg_str("abc") 是【逐字节】发出去的，一整个字符串要花几十微秒。
 *   如果两个任务同时打印，字节就会交错，输出变成这样：
 *       [0012s] T=25.31C H=48.2[0012s] STORE seq=13 ok
 *                      3% P=1013.25hPa
 *   这叫"输出串台"。它不影响功能，但会让你完全无法用日志排查问题 ——
 *   而排查问题正是日志存在的唯一理由。
 *
 * 【解决办法】
 *   一个"打印"要当成临界区：整条消息要么全发出去，要么一个字节都不发。
 *   这里用互斥锁（mutex）实现。
 *
 *   为什么用互斥锁而不是二值信号量：互斥锁带【优先级继承】——
 *   如果低优先级任务正持有锁，高优先级任务来取锁时，
 *   系统会临时把低优先级任务的优先级提到和等待者一样高，
 *   让它尽快跑完释放锁，避免"优先级反转"（中优先级任务把持 CPU
 *   导致高优先级任务无限等待）。信号量没有这个机制。
 *
 * 【用法】（用 begin/end 包住一整段输出）
 *     log_begin();
 *     log_str("[");
 *     log_u32(t);
 *     log_str("s] T=");
 *     log_i32(temp);
 *     log_str("C\r\n");
 *     log_end();
 * ============================================================ */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>

/* 建立互斥锁。必须在任何任务创建之前调用一次。 */
void log_init(void);

/* 取得 / 释放打印权。APP_MUTEX_ENABLE = 0 时两者都是空操作。 */
void log_begin(void);
void log_end(void);

/* 与 uart_dbg_* 同名同义的发送函数（本身不加锁，由 begin/end 负责）*/
void log_str(const char *s);
void log_u32(uint32_t v);
void log_i32(int32_t v);
void log_hex8(uint8_t b);
void log_hex32(uint32_t v);
void log_hex_buf(const uint8_t *buf, uint32_t len);
void log_nl(void);

/* 诊断：互斥锁被持有过的最长时间（µs，DWT 计时）。
 * 这是"临界区有多长"的量化依据 —— 临界区越短越好，
 * 因为它会阻塞所有等锁的任务。 */
uint32_t log_max_hold_us(void);

#endif /* LOG_H */
