# PC 端校验工具

> **不懂 Python？先看 [`00_白话使用说明.md`](00_白话使用说明.md)** —— 那份文档讲清楚了
> "为什么要有 Python""你只需要做什么"，全程不用你写一行代码。

> 这些脚本是简历上两句原话的**验证器**：
> - 「温湿压结果经 **PC 端复算逐位一致**」
> - 「打通 I2C 采集 → SPI 存储 → UART 上传 → **PC 逐字节校验**的完整闭环」

**没有这些工具，"闭环"这个词就只是形容词。** 有了它们，"校验通过"是一个可以打印出来的结论。

---

## ⭐ 最省事的用法：一句话菜单

不用记任何参数，选数字就行：

```
python tools\run.py
```

菜单把下面这些脚本包好了，每个选项跑完还会告诉你「该看哪一行」「什么算通过」：

| 选项 | 作用 | 对应简历 |
|---|---|---|
| 1 | 检查环境 | — |
| 2 | **用假日志演练（不需要硬件）** ← 建议第一件事 | — |
| 3 | 温湿压 PC 端复算逐位一致 | 第 3 条 |
| 4 | 32 B 记录 PC 逐字节双 CRC | 第 4 条 |
| 5 | 算栈水位压缩比 | 第 1 条的 `[实测后填]` |
| 6 | W4 全量自检 | 第 4 条 |
| 7 | 对 CRC 标准检查值 | 前提 |

它会**自动记住你的 COM 口**（存在 `tools/.last_port`），下次不用重填。

> 下面各节是**手动用法**。等菜单用熟了，或者你要把某条命令写进脚本/文档里，再看这些。

---

## 文件清单

| 文件 | 作用 | 需要硬件吗 |
|---|---|---|
| **`00_白话使用说明.md`** | **给不懂 Python 的人看的**：Python 干什么用、你只需要做什么 | 否 |
| **`run.py`** | **傻瓜菜单**（一句话启动，选数字） | 看选项 |
| **`multibus.py`** | 共用模块：CRC-16/MODBUS + 32 B 记录格式 + **BME280 补偿算法的独立 Python 实现** | 否 |
| `crc16_modbus.py` | CRC 自检 / 任意数据算 CRC | 否 |
| **`golden_vectors.py`** | ★ **用真板子实测数据做基准向量**，校验 PC 端补偿算法。**改过 `multibus.py` 之后必须跑** | 否 |
| `make_sample_log.py` | **造假日志**，没板子也能先跑通记录解析/断电分类 | 否 |
| `serial_capture.py` | 串口抓包 + 自动下发命令序列（**需要 pyserial**） | 是 |
| `verify_bme280.py` | BME280 复算逐位比对 | 是（用抓下来的日志） |
| `parse_records.py` | 32 B 记录逐条双 CRC 校验，可导出 CSV | 是（同上） |
| `stack_budget.py` | 栈水位 → 重分配 → 算压缩比（填简历那个 `[实测后填]`） | 是（同上） |

依赖：Python 3.8+；只有 `serial_capture.py` 需要 `pip install pyserial`。
**你本机已就绪**：`python` → `C:\Python311\python.exe`（3.11.9），且 **pyserial 3.5 已安装**。

---

## 一、先用假日志把工具链跑通（**推荐第一步，不需要硬件**）

```bash
python crc16_modbus.py --selftest
# 期望： 结果 : PASS     （0x4B37，CRC 界的标准检查值）

python make_sample_log.py demo.txt --records 40 --with-powerloss
python verify_bme280.py demo.txt
# 期望： 结论：PASS —— PC 端独立复算与固件输出【逐位一致】

python parse_records.py demo.txt
# 期望： 有效记录 40，半写 1，未收尾 1
#        结论: 有异常记录（正常 —— 那两条是故意造的断电场景）

python stack_budget.py demo.txt --alloc acq=256,proc=256,comm=384
# 注意：demo.txt 里没有 #STK 行，这个会报错 —— 它需要真板子的日志
```

> `--with-powerloss` 会故意造两条异常记录，用来验证**双 CRC 的分类能力**：
> 一条「数据完整但未收尾」（尾 CRC = 0xFFFF），一条「半写」。
> 看到它们被**分别**归到 `未收尾` 和 `半写` 两类，就说明分类逻辑是对的 ——
> **这正是双 CRC 相对单 CRC 的价值所在。**

### 假日志能证明什么、不能证明什么

| ✅ 能 | ❌ 不能 |
|---|---|
| 你的 PC 工具链（解析 + 比对逻辑）是通的 | **不能**证明固件里的 C 实现在认真工作 |
| 记录格式的字段顺序、CRC 覆盖范围自洽 | 不能替代上板验证 |

**必须说清楚这一点**，否则容易自我安慰。
固件和 PC 是不是同一套算法，要靠**开机横幅里的 `#CRC` 行**来对：

```
固件：#CRC,check='123456789',got=0x4B37,expect=0x4B37,PASS
PC  ：python crc16_modbus.py --selftest   →   结果 : PASS (0x4B37)
```

**两边的 `got` 必须相同。** 不同的话后面所有校验都会"全军覆没"，
而且看起来会非常像"Flash 坏了" —— **先排除算法问题，再查硬件。**

---

## 二、上板后的完整流程

### 2.1 抓校准系数 + 测量样本（验证「复算逐位一致」）

```bash
# c = 打印 33 个校准系数；m = 触发一次测量并打印原始量
python serial_capture.py --port COM5 --out bme.txt --seq c,m,m,m,m,m --gap 2

python verify_bme280.py bme.txt
```

**期望输出**：

```
解析到校准系数 18 项（覆盖全部 33 个：其中 H4/H5 是 12 位拼接值）
解析到测量样本 5 个

复算比对：共 30 项，其中不一致 0 项
结论：PASS —— PC 端独立复算与固件输出【逐位一致】
```

**记住这个数字（`5 个样本 × 6 个量 = 30 项`），面试/论文里可以直接引用。**

### 2.2 抓 W4 自检

```bash
python serial_capture.py --port COM5 --out selftest.txt \
       --seq t --stop-marker "W4 自检结束"
```

自检包含：四种 SPI 模式回环 / 分模式读 W25Q64 ID / 32 B 记录闭环 / 断电三情况。
判据见 `docs/05_验收手册.md` §6.1。

### 2.3 回读全部记录并逐字节校验（验证「完整闭环」）

```bash
python serial_capture.py --port COM5 --out records.txt \
       --seq r --stop-marker "#DUMP,end"

python parse_records.py records.txt --csv records.csv --show 5
```

**期望输出**：

```
校验结果
  有效记录 (头尾 CRC 都通过) : N
  半写记录 (头 CRC 失败)      : 0
  未收尾   (头过、尾失败)     : 0
  序号连续性: 全部连续，无整条丢失
结论: PASS —— 全部 N 条记录的头、尾 CRC 均在 PC 端独立重算通过
```

`--csv` 导出的文件带 UTF-8 BOM，**Excel 双击直接能打开**（有时间列、温度列…），
可以直接拿去画图放进论文。

> **回读耗时参考**：每条记录打印 `REC,<idx>,<64位HEX>` ≈ 75 字节。
> 115200 bps 下大约 **1.5 KB/s**，所以 1000 条 ≈ 50 秒，10000 条 ≈ 8 分钟。
> 记录区满容量（131072 条）全倒出来要 **1.5 小时以上** —— 验收时用几百到几千条就够。

### 2.4 算栈压缩比（填简历那个占位符）

```bash
# 先跑一段（越久越准），让固件打出 #STK 行
python serial_capture.py --port COM5 --out long_run.txt --seconds 300
# （期间可以发几次 s 命令立即打印，不用等 60 秒自动那一行）

python stack_budget.py long_run.txt --alloc acq=256,proc=256,comm=384 --baseline 512
```

**它会直接输出可以粘进简历的句子**，并给出推荐的新栈尺寸。

> ⚠️ **三条硬规矩**：
> ① `--alloc` 必须和**当时烧录的固件一致**（开机横幅 `#BUILD,APP_STACK_MEASURED=` 可佐证）
> ② 压缩比**必须说明相对哪个基准**（默认是"每任务统一 512 字"）
> ③ **不许用示例值** —— 必须是你自己跑出来的数

---

## 三、`multibus.py` 里那两个"必须按 C 语义实现"的地方

如果你要改这个模块，这两处**千万别"顺手改成 Python 写法"**：

### ① C 的整数除法是"向零截断"，Python 的 `//` 是"向下取整"

```python
#   C:      -7 / 2  = -3
#   Python: -7 // 2 = -4
def c_div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q
```

气压补偿里 `(p << 31) - var2` 完全可能是负数。不处理的话 PC 算出来会**差 1** ——
而且看起来像随机误差，最难查。

### ② 32 位溢出要显式模拟

```python
def i32(x):                      # 按 C 的 int32_t 截断
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x >= 0x80000000 else x
```

温度补偿路径全是 `int32` 运算，中间量理论上会溢出回绕。
Python 的整数是任意精度，不回绕 —— 所以要手动加。

> **这两条的存在本身就是个信号**：说明"复算逐位一致"不是随便写个脚本就行的，
> 你得先搞清楚 C 的整数语义。**这也是面试可以讲的点。**

---

## 四、命令速查

```bash
# CRC
python crc16_modbus.py --selftest
python crc16_modbus.py --ascii "123456789" --low-first
python crc16_modbus.py --hex "A5 00 01 02"

# 假日志（不需要硬件）
python make_sample_log.py demo.txt --records 40 --with-powerloss

# 串口
python serial_capture.py --list                                  # 列出可用串口
python serial_capture.py --port COM5 --out log.txt --seconds 30   # 只抓 30 秒
python serial_capture.py --port COM5 --out log.txt --seq c,m,m,m  # 发命令序列
python serial_capture.py --port COM5 --out log.txt --seq r --stop-marker "#DUMP,end"

# 校验
python verify_bme280.py log.txt --verbose
python parse_records.py log.txt --csv out.csv --show 10
python stack_budget.py log.txt --alloc acq=256,proc=256,comm=384 --baseline 512
```

---

## 五、固件侧对应的串口命令

| 命令 | 产出什么 | 哪个脚本消费 |
|---|---|---|
| `c` | `#CALIB,...`（33 个校准系数） | `verify_bme280.py` |
| `m` | `#RAW` / `#CMP` / `#OUT` | `verify_bme280.py` |
| `r` | `REC,<idx>,<64位HEX>` 全量记录 | `parse_records.py` |
| `v` | `#VERIFY,...`（板子自己校验的分类统计） | 人工看 |
| `t` | W4 全量自检 | 人工看 / 存档 |
| `s` | `#STK,...`（栈水位 + 堆余量 + 锁诊断） | `stack_budget.py` |
| `p` | `#STA,...`（运行状态汇总） | 人工看 |

发命令可以用 `serial_capture.py --seq`，也可以直接用串口助手手动敲（单字符 + 回车）。
