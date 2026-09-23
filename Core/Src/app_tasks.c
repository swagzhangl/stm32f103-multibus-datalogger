/* ============================================================
 * app_tasks.c —— 三任务采集架构（W3 / W3.5 的落地产物）
 *
 * 这个文件就是简历上"基于 FreeRTOS 构建 3 任务采集架构"那句话的本体：
 *   · 采集任务用 vTaskDelayUntil 保证 1 s 周期不漂移
 *   · 消息队列解耦采集与存储速率差
 *   · 互斥锁保护 I2C / SPI / UART 共享资源
 *   · 二值信号量传递"报警发生"这个事件
 *   · uxTaskGetStackHighWaterMark 实测各任务栈水位
 * ============================================================ */
#include "app_tasks.h"
#include "app_config.h"
#include "board.h"
#include "log.h"
#include "uart_dbg.h"
#include "bme280.h"
#include "recorder.h"
#include "buzzer.h"
#include "i2c_soft.h"
#include "spi_soft.h"
#include "self_test.h"
#include "delay_us.h"
#include "clock_init.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ============================================================
 * 任务栈尺寸
 *
 * 【为什么必须分两档】
 *   栈是 20 KB RAM 里最大的一块可变开销，但"先猜一个小数"是纯粹的赌博。
 *   正确做法是先给宽裕值跑起来，读实测水位，再按峰值 ×1.5 收紧。
 *   这个"实测 → 收紧"的过程，就是简历上那句
 *   "用 uxTaskGetStackHighWaterMark 实测各任务栈水位并重新分配栈空间"
 *   要填的那个压缩比 （［实测后填］）。
 * ============================================================ */
#if APP_STACK_MEASURED
/* ★★ 待填：把下面三个值换成【你实测出来的峰值 × 1.5】（单位：字）★★
 *
 * 不要照抄这里的数字 —— 它们只是"宽裕档"的占位，留着这里的
 * #warning 就是为了防止你在没实测的情况下直接把它当成实测值。
 * 实测步骤见 docs/05_验收手册.md 第 3 节。 */
#warning "APP_STACK_MEASURED=1：请先把 STK_ACQ / STK_PROC / STK_COMM 换成实测峰值 x1.5"
#define STK_ACQ     256U
#define STK_PROC    256U
#define STK_COMM    384U
#else
/* 宽裕档：先跑起来。1 字 = 4 字节，所以 256 字 = 1 KB。
 * 目标不是"够用"，是"绝对不溢出"—— 先保证正确，再谈节省。 */
#define STK_ACQ     256U
#define STK_PROC    256U
#define STK_COMM    384U
#endif

/* ---------- 优先级 ---------- */
#define PRIO_ACQ    4U      /* 采集最高：周期必须准 */
#define PRIO_PROC   3U      /* 处理居中 */
#define PRIO_COMM   2U      /* 通信最低：慢一点没人受伤 */

/* ---------- 队列深度 ---------- */
#define Q_RAW_LEN   8U
#define Q_REP_LEN   8U

/* ---------- 取锁超时 ----------
 * 用有限超时而不是 portMAX_DELAY：
 * 万一某处持锁后卡住，有限超时能让系统"降级继续跑"并留下计数，
 * 而不是整个系统静静地死锁在那里。 */
#define LOCK_TMO_MS     200U

/* ============================================================
 * 文件级状态
 * ============================================================ */
static QueueHandle_t     s_q_raw     = NULL;   /* 采集 → 处理 */
static QueueHandle_t     s_q_rep     = NULL;   /* 处理 → 通信 */
static SemaphoreHandle_t s_mtx_i2c   = NULL;
static SemaphoreHandle_t s_mtx_spi   = NULL;
static SemaphoreHandle_t s_sem_alarm = NULL;

static TaskHandle_t s_h_acq  = NULL;
static TaskHandle_t s_h_proc = NULL;
static TaskHandle_t s_h_comm = NULL;

static volatile uint32_t s_seconds      = 0U;   /* 运行秒数 = 记录时间戳 */
static volatile uint8_t  s_alarm_state  = 0U;
static volatile uint32_t s_drop_raw     = 0U;
static volatile uint32_t s_drop_rep     = 0U;
static volatile uint32_t s_lock_timeout = 0U;

/* ---------- LED（PC13，板载，低电平点亮）----------
 * 用 LED 而不是示波器来观察"周期有没有漂移"：
 * 让它每 10 秒翻转一次，眼睛就能看出节拍是否均匀。
 * 更严格的抖动测量用逻辑分析仪抓这个脚的跳变间隔。 */
static void led_init(void)
{
    /* PC13 挂在"备份域"，复位后默认功能是 TAMPER-RTC 而不是普通 IO，
     * 必须先解锁备份域并关掉 TAMPER 功能，否则怎么配都不亮。 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPCEN;
    RCC->APB1ENR |= RCC_APB1ENR_BKPEN | RCC_APB1ENR_PWREN;
    PWR->CR      |= PWR_CR_DBP;                 /* DBP = 1，解除备份域写保护 */
    BKP->CR      &= ~BKP_CR_TPE;                /* TPE = 0，关 TAMPER */

    GPIOC->CRH &= ~(0xFUL << 20);               /* 清 PC13 那 4 位 */
    GPIOC->CRH |=  (0x2UL << 20);               /* 通用推挽输出 2 MHz */
    GPIOC->BSRR = (1UL << 13);                  /* 先熄灭（低电平点亮）*/
}

static void led_toggle(void)
{
    /* ODR 异或在这里是安全的：PC13 只被这一个任务碰。
     * 若同一引脚被多处操作，就必须改用 BSRR/BRR 的原子写。 */
    GPIOC->ODR ^= (1UL << 13);
}

/* ============================================================
 * 共享总线互斥锁
 * ============================================================ */
uint8_t app_lock_i2c(uint32_t timeout_ms)
{
    if (s_mtx_i2c == NULL) { return 1U; }
    if (xSemaphoreTake(s_mtx_i2c, pdMS_TO_TICKS(timeout_ms)) != pdPASS)
    {
        s_lock_timeout++;
        return 0U;
    }
    return 1U;
}

void app_unlock_i2c(void)
{
    if (s_mtx_i2c != NULL) { (void)xSemaphoreGive(s_mtx_i2c); }
}

uint8_t app_lock_spi(uint32_t timeout_ms)
{
    if (s_mtx_spi == NULL) { return 1U; }
    if (xSemaphoreTake(s_mtx_spi, pdMS_TO_TICKS(timeout_ms)) != pdPASS)
    {
        s_lock_timeout++;
        return 0U;
    }
    return 1U;
}

void app_unlock_spi(void)
{
    if (s_mtx_spi != NULL) { (void)xSemaphoreGive(s_mtx_spi); }
}

uint32_t app_get_seconds(void)      { return s_seconds; }
uint32_t app_get_drop_raw(void)     { return s_drop_raw; }
uint32_t app_get_drop_rep(void)     { return s_drop_rep; }
uint32_t app_get_lock_timeouts(void){ return s_lock_timeout; }

/* ============================================================
 * 任务 1：采集（优先级最高，1 s 精确周期）
 * ============================================================ */
static void task_acq(void *arg)
{
    TickType_t     last = xTaskGetTickCount();
    bme280_data_t  d;
    app_sample_t   s;
    uint32_t       seq = 0U;
    uint8_t        rd;

    (void)arg;

    for (;;)
    {
        s.seq         = seq;
        s.timestamp_s = s_seconds;
        s.used        = 0U;
        s.alarm       = 0U;
        s.store_rc    = 0xFFU;
        s.temp_centi  = 0;
        s.humi_centi  = 0U;
        s.press_pa    = 0U;

        /* --- 读传感器（I2C 是共享资源，取锁）--- */
        if (app_lock_i2c(LOCK_TMO_MS) != 0U)
        {
            rd = bme280_read(&d);
            app_unlock_i2c();

            if (rd == 0U)
            {
                s.temp_centi = d.temperature;
                s.humi_centi = d.humidity;
                s.press_pa   = d.pressure;
                s.used       = 1U;
            }
        }

        /* 读失败不阻塞、不重试、不写坏数据 —— 直接标记 used=0 交给下游，
         * 由处理任务跳过落库。绝不把已知不可信的数据写进 Flash。 */

        /* --- 交给处理任务。
         *     队列满 = 处理速度跟不上采集速度 → 丢弃本样本并【计数】。
         *     有统计才叫工程实现：丢数据不可怕，丢了不知道才可怕。 --- */
        if (xQueueSend(s_q_raw, &s, 0) != pdPASS) { s_drop_raw++; }

        seq++;
        s_seconds++;

        /* 每 10 个周期翻转一次 PC13（板载 LED），周期 10 秒。
         * 作用：眼睛就能看出节拍有没有漂；要量化抖动就用逻辑分析仪
         * 抓 PC13 跳变的时间间隔，和 10.000 s 比对。 */
        if ((seq % 10UL) == 0UL) { led_toggle(); }

        /* ★★ 关键：vTaskDelayUntil 而不是 vTaskDelay ★★
         *
         *   vTaskDelay(t)      = "从现在起再等 t"  → 本轮耗时会被累加进周期，
         *                        跑久了周期越飘越大（1 s 变成 1.02 s、1.05 s…）
         *   vTaskDelayUntil(&last, t) = "到 next 这个绝对时刻再叫我"
         *                        → 以固定节拍推进，【不累积漂移】
         *
         *   周期任务必须用后者。这是 FreeRTOS 面试的必问题，
         *   而"为什么"的答案就是上面这两行。
         *
         *   代价：如果某轮真的超时了（比如 Flash 擦除卡了 400 ms），
         *   vTaskDelayUntil 会立刻返回以追赶进度 —— 这正是我们要的语义：
         *   宁可某轮被压缩，也不让整体节拍往后漂。 */
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000U));
    }
}

/* ============================================================
 * 任务 2：处理（报警判定 + 打包落库）
 * ============================================================ */
static void task_proc(void *arg)
{
    app_sample_t s;
    rec_data_t   rec;
    uint8_t      rc;
    uint8_t      alarm_now;
    uint8_t      alarm_prev = 0U;

    (void)arg;

    for (;;)
    {
        if (xQueueReceive(s_q_raw, &s, portMAX_DELAY) != pdPASS) { continue; }

        /* --- 报警判定，带回差 ---
         * 纯 RAM 运算，不碰总线，所以不需要加锁。 */
        alarm_now = buzzer_alarm_update(s.used != 0U ? s.humi_centi : 0U, &s_alarm_state);
        s.alarm   = alarm_now;

        /* --- 报警从"关"变"开"的那一刻：用二值信号量通知通信任务 ---
         *
         * ★ 这里就是"队列 vs 信号量"的分界线：
         *   队列传【数据】—— 有长度、有缓冲、取走就没了；
         *   信号量传【事件】—— 只关心"发生过"，不关心内容。
         *   报警要通知的是"湿度超限了"这件事，内容由状态变量承载，
         *   所以信号量才是对的工具。 */
        if ((alarm_now != 0U) && (alarm_prev == 0U))
        {
            (void)xSemaphoreGive(s_sem_alarm);
        }
        alarm_prev = alarm_now;

        /* --- 打包 + 落库（SPI 是共享资源，取锁）--- */
        s.store_rc = 0xFFU;
        if (s.used != 0U)
        {
            rec.seq         = s.seq;
            rec.timestamp_s = s.timestamp_s;
            rec.temperature = (int32_t)(s.temp_centi * (int32_t)APP_TEMP_SCALE);
            rec.humidity    = s.humi_centi * APP_HUMI_SCALE;
            rec.pressure_pa = s.press_pa;
            rec.adc1        = 0U;      /* 预留通道，11 月模块 3 填 */
            rec.adc2        = 0U;

            if (app_lock_spi(LOCK_TMO_MS) != 0U)
            {
                rc = recorder_append(&rec);
                app_unlock_spi();
                s.store_rc = rc;
            }
        }

        if (xQueueSend(s_q_rep, &s, 0) != pdPASS) { s_drop_rep++; }
    }
}

/* ============================================================
 * 任务 3：通信（串口上报 + 命令处理）
 * ============================================================ */
#define CMD_LINE_MAX    24U

static void print_stack_report(void)
{
    log_begin();
    log_str("#STK,heap_free=");
    log_u32((uint32_t)xPortGetFreeHeapSize());
    log_str(",tasks=");
    log_u32((uint32_t)uxTaskGetNumberOfTasks());
    log_str(",acq=");
    log_u32((uint32_t)uxTaskGetStackHighWaterMark(s_h_acq));
    log_str(",proc=");
    log_u32((uint32_t)uxTaskGetStackHighWaterMark(s_h_proc));
    log_str(",comm=");
    log_u32((uint32_t)uxTaskGetStackHighWaterMark(s_h_comm));
    log_str(",uart_hold_us=");
    log_u32(log_max_hold_us());
    log_str(",lock_tmo=");
    log_u32(s_lock_timeout);
    log_nl();
    log_end();
}

static void print_help(void)
{
    log_begin();
    log_str("#CMD,h=help s=stack c=calib m=measure r=dump-records v=verify\r\n");
    log_str("#CMD,t=selftest-all i=chipid e=erase(needs twice) p=status\r\n");
    log_end();
}

static void print_status(void)
{
    log_begin();
    log_str("#STA,uptime=");
    log_u32(s_seconds);
    log_str("s,records=");
    log_u32(recorder_count());
    log_str(",capacity=");
    log_u32(recorder_capacity());
    log_str(",alarm=");
    log_u32((uint32_t)s_alarm_state);
    log_str(",drop_raw=");
    log_u32(s_drop_raw);
    log_str(",drop_rep=");
    log_u32(s_drop_rep);
    log_str(",bme_rej=");
    log_u32(bme280_get_reject_count());
    log_str(",i2c_nack=");
    log_u32(i2c_soft_nack_count());
    log_nl();
    log_end();
}

/* ---------- 清空记录区需要确认两次，避免手滑 ---------- */
static uint8_t  s_erase_armed = 0U;
static uint32_t s_erase_arm_t = 0U;

/* ★ 两次确认之间必须至少间隔 ERASE_MIN_GAP_S 秒。
 *
 * 【为什么必须有这个下限 —— 实测踩出来的】
 *   用户的串口终端会把一次发送重复送两次（实测：发一次 'c'，收到了两份 #CALIB）。
 *   原来的判据是 `0 <= (s_seconds - s_erase_arm_t) <= 15`，而 s_seconds 由采集任务
 *   每秒递增一次、不是精确时钟。于是同一秒内的两次 'e' 会算出 dt = 0，
 *   直接满足"15 秒内"→ 当场擦除。
 *   后果：终端重复发送一次，就等于【一次按键擦掉全部记录】——
 *   那是毕设的实测数据，赔不起。
 *
 *   加了 dt >= 2 s 这个下限后，重复发送只会得到一句"太快了"，
 *   必须人手真的间隔两秒以上按第二次才会执行。
 *
 * 【副作用】正常操作要多等 2 秒。这是有意的取舍：
 *   擦除是不可逆的，宁可慢一点。 */
#define ERASE_MIN_GAP_S   2U
#define ERASE_WINDOW_S   15U

static void handle_erase(void)
{
    if (s_erase_armed != 0U)
    {
        uint32_t dt = s_seconds - s_erase_arm_t;

        /* 太快 —— 多半是终端重复发送，不是人手真的按了两次。
         * 这里【保持武装状态、也不刷新计时起点】，所以随手重发不会把
         * 确认窗口一直"续命"。 */
        if (dt < ERASE_MIN_GAP_S)
        {
            log_begin();
            log_str("#ERASE,TOO_SOON,请间隔 ≥");
            log_u32(ERASE_MIN_GAP_S);
            log_str(" 秒后再发一次 e（仍处于待确认）\r\n");
            log_end();
            return;
        }

        if (dt <= ERASE_WINDOW_S)
        {
            uint32_t rc;

            s_erase_armed = 0U;

            if (app_lock_spi(LOCK_TMO_MS) != 0U)
            {
                rc = recorder_erase_all();
                app_unlock_spi();

                log_begin();
                log_str("#ERASE,");
                if (rc == 0U) { log_str("OK,records=0"); }
                else          { log_str("FAIL,sector="); log_u32(rc); }
                log_nl();
                log_end();
            }
            else
            {
                /* 取不到 SPI 锁必须报出来 —— 静默失败会让人以为"擦成功了" */
                log_begin();
                log_str("#ERASE,ERR,lock timeout（未执行）\r\n");
                log_end();
            }
            return;
        }

        /* dt > 窗口 → 掉到下面重新武装 */
    }

    s_erase_armed = 1U;
    s_erase_arm_t = s_seconds;
    log_begin();
    log_str("#ERASE,ARMED,请再发一次 e 确认（");
    log_u32(ERASE_WINDOW_S);
    log_str(" 秒内有效）\r\n");
    log_end();
}

static void cmd_dispatch(char c)
{
    switch (c)
    {
    case 'h':
    case '?': print_help();                 break;
    case 's': print_stack_report();         break;
    case 'p': print_status();               break;
    case 'c': selftest_bme280_calib_dump(); break;
    case 'm': selftest_bme280_measure_dump(); break;
    case 'r': selftest_records_dump();      break;
    case 'v': selftest_records_verify();    break;
    case 't': selftest_run_all();           break;
    case 'i':
        /* 读一次 chip id —— 走 I2C，和采集任务抢同一把锁。
         * 这个命令存在的意义，就是让 I2C 互斥锁在现实中真的被争用。 */
        if (app_lock_i2c(LOCK_TMO_MS) != 0U)
        {
            uint8_t id = bme280_get_chip_id();
            app_unlock_i2c();
            log_begin();
            log_str("#CHIPID,0x");
            log_hex8(id);
            log_str((id == 0x60U) ? ",BME280(有湿度)" : ",非 BME280");
            log_nl();
            log_end();
        }
        else
        {
            log_begin(); log_str("#CHIPID,ERR,lock timeout\r\n"); log_end();
        }
        break;
    case 'e': handle_erase();               break;
    default:
        /* 忽略未知字符（含回车换行的残留），不报错 —— 串口终端会发很多
         * 控制字符，逐个报错只会把有用的输出淹掉。 */
        break;
    }
}

static void task_comm(void *arg)
{
    app_sample_t s;
    uint8_t      ch;
    char         line[CMD_LINE_MAX];
    uint8_t      n = 0U;
    uint32_t     rep_count = 0U;
    uint32_t     last_stack_report = 0U;

    (void)arg;

    for (;;)
    {
        /* --- 收上报数据。
         *     用 20 ms 超时而不是 portMAX_DELAY：这样任务会周期性醒来
         *     去轮询串口命令，不需要为命令单开一个中断通知机制。
         *     这是"一个任务兼两件事"时最省事的写法，代价是命令响应
         *     最多延迟 20 ms —— 对人手动敲命令来说完全无感。 --- */
        if (xQueueReceive(s_q_rep, &s, pdMS_TO_TICKS(20)) == pdPASS)
        {
            rep_count++;

#if APP_UART_VERBOSE
            /* 详略开关：1 = 每条都打（互斥锁对照实验用）；
             *           0 = 每 60 条打一行（24 小时长跑用）*/
            {
                log_begin();
                log_str("[");
                log_u32(s.timestamp_s);
                log_str("s] seq=");
                log_u32(s.seq);
                if (s.used != 0U)
                {
                    log_str(" T=");
                    log_i32(s.temp_centi);
                    log_str("c H=");
                    log_u32(s.humi_centi);
                    log_str("c P=");
                    log_u32(s.press_pa);
                    log_str("Pa");
                }
                else
                {
                    log_str(" SENSOR_INVALID");
                }
                log_str(" store=");
                if (s.store_rc == 0U)        { log_str("ok"); }
                else if (s.store_rc == 0xFFU){ log_str("skip"); }
                else                         { log_str("ERR"); log_u32(s.store_rc); }
                log_nl();
                log_end();
            }
#else
            if ((rep_count % 60U) == 0U)
            {
                log_begin();
                log_str("[");
                log_u32(s.timestamp_s);
                log_str("s] seq=");
                log_u32(s.seq);
                log_str(" records=");
                log_u32(recorder_count());
                log_nl();
                log_end();
            }
#endif
        }

        /* --- 报警事件（二值信号量）--- */
        if ((s_sem_alarm != NULL) &&
            (xSemaphoreTake(s_sem_alarm, 0) == pdPASS))
        {
            log_begin();
            log_str("!!! ALARM humidity >= ");
            log_u32(ALARM_HUMI_ON / 100U);
            log_str(".00 %RH at t=");
            log_u32(s_seconds);
            log_str("s\r\n");
            log_end();
        }

        /* --- 串口命令：不阻塞地取字节，攒成行再分发 --- */
        while (uart_dbg_rx_pop(&ch) != 0U)
        {
            if ((ch == '\r') || (ch == '\n'))
            {
                if (n > 0U)
                {
                    line[n] = '\0';
                    cmd_dispatch(line[0]);   /* 本版只认单字符命令 */
                    n = 0U;
                }
            }
            else if (n < (CMD_LINE_MAX - 1U))
            {
                line[n] = (char)ch;
                n++;
            }
            else
            {
                n = 0U;      /* 超长输入直接丢掉，防止缓冲区溢出的边角 */
            }
        }

        /* --- 每 60 秒自动报一次栈水位（长跑时的主要观测手段）--- */
        if ((s_seconds - last_stack_report) >= 60U)
        {
            last_stack_report = s_seconds;
            print_stack_report();

            if (recorder_count() >= recorder_capacity())
            {
                log_begin();
                log_str("#WARN,record area FULL, stop logging\r\n");
                log_end();
            }
        }
    }
}

/* ============================================================
 * 启动
 * ============================================================ */
uint8_t app_tasks_start(void)
{
    /* log_init 必须最先 —— 后面每一句诊断打印都要用到它的互斥锁 */
    log_init();
    led_init();

    /* 三条总线的物理层初始化。
     * ★ 串口由 main() 负责初始化（它要在建任务之前先打启动横幅），
     *   这里不重复做，避免两处初始化让人搞不清顺序。
     * ★ spi_soft_init 一个都不能漏：漏了的话 SPI 引脚没配成输出，
     *   W25Q64 读回来永远是 0xFF 或 0x00，看起来像"器件坏了"。 */
    buzzer_init();
    i2c_soft_init();
    spi_soft_init();

    /* FreeRTOS heap 只有 9 KB 左右，任何一个对象创建失败都必须当场报出来，
     * 否则后面会在某个奇怪的地方崩，排查成本高一个数量级。 */
    s_q_raw = xQueueCreate(Q_RAW_LEN, sizeof(app_sample_t));
    s_q_rep = xQueueCreate(Q_REP_LEN, sizeof(app_sample_t));
    if ((s_q_raw == NULL) || (s_q_rep == NULL)) { return 1U; }

    s_mtx_i2c = xSemaphoreCreateMutex();
    s_mtx_spi = xSemaphoreCreateMutex();
    if ((s_mtx_i2c == NULL) || (s_mtx_spi == NULL)) { return 2U; }

    s_sem_alarm = xSemaphoreCreateBinary();
    if (s_sem_alarm == NULL) { return 3U; }

    /* 串口 + 传感器初始化放这里，放在任务创建之前，
     * 这样第一个采集周期开始时所有外设都已就绪。 */
    if (bme280_init() != 0U)
    {
        log_begin();
        log_str("#WARN,bme280 init FAILED (检查 0x76/0x77 地址与上拉)\r\n");
        log_end();
    }

    if (xTaskCreate(task_acq,  "acq",  STK_ACQ,  NULL, PRIO_ACQ,  &s_h_acq)  != pdPASS) { return 4U; }
    if (xTaskCreate(task_proc, "proc", STK_PROC, NULL, PRIO_PROC, &s_h_proc) != pdPASS) { return 5U; }
    if (xTaskCreate(task_comm, "comm", STK_COMM, NULL, PRIO_COMM, &s_h_comm) != pdPASS) { return 6U; }

    return 0U;
}
