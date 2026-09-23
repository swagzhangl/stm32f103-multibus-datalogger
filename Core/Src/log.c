/* ============================================================
 * log.c —— 互斥锁保护的串口打印
 * ============================================================ */
#include "log.h"
#include "uart_dbg.h"
#include "delay_us.h"
#include "app_config.h"

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_mtx      = NULL;
static uint32_t          s_max_hold = 0U;
static uint32_t          s_hold_t0  = 0U;

void log_init(void)
{
    if (s_mtx == NULL)
    {
        s_mtx = xSemaphoreCreateMutex();
    }
}

void log_begin(void)
{
#if APP_MUTEX_ENABLE
    if (s_mtx != NULL)
    {
        (void)xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_hold_t0 = delay_now_us();
    }
#endif
}

void log_end(void)
{
#if APP_MUTEX_ENABLE
    if (s_mtx != NULL)
    {
        /* 记一次"这把锁被持有多久"，用来量化临界区长度。
         * 临界区应该尽量短 —— 它每多 1 ms，所有等锁的任务就多等 1 ms。 */
        uint32_t held = delay_elapsed_us(s_hold_t0);
        if (held > s_max_hold) { s_max_hold = held; }

        (void)xSemaphoreGive(s_mtx);
    }
#endif
}

uint32_t log_max_hold_us(void)
{
    return s_max_hold;
}

/* ---------- 以下都是"裸"发送，锁由 begin/end 负责 ---------- */

void log_str(const char *s)              { uart_dbg_str(s); }
void log_u32(uint32_t v)                 { uart_dbg_u32(v); }
void log_i32(int32_t v)                  { uart_dbg_i32(v); }
void log_hex8(uint8_t b)                 { uart_dbg_hex8(b); }
void log_hex32(uint32_t v)               { uart_dbg_hex32(v); }
void log_hex_buf(const uint8_t *b, uint32_t n) { uart_dbg_hex_buf(b, n); }
void log_nl(void)                        { uart_dbg_nl(); }
