#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
multibus.py —— 多总线数据采集终端 · PC 端算法参考实现（共用模块）

【这个文件为什么存在】
  简历上写"温湿压结果经 PC 端复算逐位一致"、"PC 逐字节校验双 CRC"。
  这两句话要成立，PC 端就必须有一份【独立实现】——
  不是把固件的输出抄一遍，而是拿"原始输入"重新算一遍，再和固件输出比对。

  所以本文件做两件事：
    ① CRC-16/MODBUS：和 C 版 crc16.c 逐位等价
    ② BME280 补偿：照抄 Bosch 官方整数版公式，但用 Python 重写一遍

【一个关键细节：C 的整数除法是"向零截断"，Python 的 // 是"向下取整"】
  对正数两者一样，对负数就不一样：
      C:      -7 / 2  = -3        （向零截断）
      Python: -7 // 2 = -4        （向下取整）
  气压补偿里 (`p << 31) - var2` 完全可能是负数，所以必须显式实现
  向零截断的除法，否则复算结果会差 1 从而"不一致"——
  而那种差 1 最难查，因为它看起来像随机误差。
"""

import struct

# ============================================================
# CRC-16/MODBUS
# ============================================================
CRC16_MODBUS_INIT = 0xFFFF
# 标准检查值：ASCII "123456789" 在 CRC-16/MODBUS 下的结果
# 这个值在 CRC 参数表里可以查到，用来证明两边算法一致
CRC16_MODBUS_CHECK = 0x4B37

# ------------------------------------------------------------
# 尾 CRC 用【另一个多项式】：CRC-16/CCITT-FALSE（poly 0x1021，不反射）
#
# 为什么不能沿用 MODBUS：
#   CRC 对初值是仿射的 crc_J(m) = crc_0(m) ^ g_len(J)。
#   只要第二条 CRC 覆盖范围 = "数据 + 第一条 CRC" 且用【同一多项式】，就有
#       尾 = g_len(init1 ^ init2)   ← 与数据无关，恒为常数
#   实测两次都成立：J=0xFFFF → 恒 0x0000；J=0x0000 → 恒 0x4FFE。
#   ⇒ 必须换多项式才能打破这个恒等式。
#
# 存储字节序：本工程统一【小端】（与记录里其它字段一致）。
# ------------------------------------------------------------
CRC16_CCITT_POLY = 0x1021
CRC16_CCITT_INIT = 0xFFFF
# 标准检查值（ASCII "123456789"），用来证明这个实现没写错
CRC16_CCITT_CHECK = 0x29B1


def crc16_modbus(data, crc=CRC16_MODBUS_INIT):
    """逐位算法，与固件 crc16.c 完全一致。

    poly 0x8005 反射后为 0xA001，init 0xFFFF，refin/refout = true。
    """
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def crc16_modbus_init(init, data):
    """指定初值重算（MODBUS）。等价于固件 crc16_modbus_init()。"""
    return crc16_modbus(data, init)


def crc16_ccitt(data):
    """CRC-16/CCITT-FALSE：poly 0x1021，init 0xFFFF，高位先行（不反射）。

    与固件 crc16.c 的 crc16_ccitt() 逐位一致。
    注意它和 MODBUS 的写法是【镜像】的：
      MODBUS：异或进低 8 位 → 判最低位 → 右移 → 异或 0xA001
      CCITT ：异或进高 8 位 → 判最高位 → 左移 → 异或 0x1021
    """
    crc = CRC16_CCITT_INIT
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC16_CCITT_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


def crc16_selftest():
    """用公认检查值验证本实现。返回 (ok, got)"""
    got = crc16_modbus(b"123456789")
    return (got == CRC16_MODBUS_CHECK, got)


# ============================================================
# 32 字节记录格式
#
#   偏移   长度  字段
#   0–3     4    记录序号        uint32 小端
#   4–7     4    时间戳(秒)      uint32 小端
#   8–11    4    温度            int32  小端，℃ × 1000
#   12–15   4    湿度            uint32 小端，%RH × 1000
#   16–19   4    气压            uint32 小端，Pa
#   20–23   4    ADC 通道 1      uint32（预留）
#   24–27   4    ADC 通道 2      uint32（预留）
#   28–29   2    头 CRC          CRC-16/MODBUS      覆盖 0–27
#   30–31   2    尾 CRC          CRC-16/CCITT-FALSE 覆盖 0–29
#
# ★ 为什么头尾两个 CRC 必须用【不同多项式】：见 CRC16_CCITT_POLY 处的说明。
#   同多项式会让尾 CRC 退化成与数据无关的常数（实测：恒 0x0000 或恒 0x4FFE）。
# ============================================================
REC_SIZE = 32
REC_DATA_LEN = 28
REC_HEAD_CRC_OFF = 28
REC_TAIL_CRC_OFF = 30

RECORD_AREA_BYTES = 4 * 1024 * 1024          # 4 MiB
REC_SLOT_MAX = RECORD_AREA_BYTES // REC_SIZE  # 131072 条
REC_PER_PAGE = 256 // REC_SIZE                # 8 条
REC_PER_SECTOR = 4096 // REC_SIZE             # 128 条

SLOT_EMPTY = 0        # 全 0xFF，从未写过
SLOT_OK = 1           # 头尾 CRC 都通过
SLOT_HALF = 2         # 头 CRC 失败 → 编程中断电
SLOT_UNFINISHED = 3   # 头 CRC 过、尾 CRC 失败 → 数据完整但未收尾

SLOT_NAME = {
    SLOT_EMPTY: "EMPTY(未写)",
    SLOT_OK: "OK(完整)",
    SLOT_HALF: "HALF(半写)",
    SLOT_UNFINISHED: "UNFINISHED(未收尾)",
}


def pack_record(seq, timestamp_s, temp_milli, humi_milli, press_pa,
                adc1=0, adc2=0):
    """打包成 32 字节。字段名里的 milli 表示"×1000"。

    temp_milli 是温度 ℃×1000（可以是负数，按 int32 小端存）
    """
    buf = bytearray(REC_SIZE)

    struct.pack_into("<I", buf, 0, seq & 0xFFFFFFFF)
    struct.pack_into("<I", buf, 4, timestamp_s & 0xFFFFFFFF)
    struct.pack_into("<i", buf, 8, temp_milli)
    struct.pack_into("<I", buf, 12, humi_milli & 0xFFFFFFFF)
    struct.pack_into("<I", buf, 16, press_pa & 0xFFFFFFFF)
    struct.pack_into("<I", buf, 20, adc1 & 0xFFFFFFFF)
    struct.pack_into("<I", buf, 24, adc2 & 0xFFFFFFFF)

    head = crc16_modbus(bytes(buf[0:REC_DATA_LEN]))
    struct.pack_into("<H", buf, REC_HEAD_CRC_OFF, head)

    # 尾 CRC：覆盖 0–29，用【另一个多项式】CRC-16/CCITT-FALSE。
    # 同多项式会退化成常数（见 CRC16_CCITT_POLY 处的说明）。
    tail = crc16_ccitt(bytes(buf[0:REC_TAIL_CRC_OFF]))
    struct.pack_into("<H", buf, REC_TAIL_CRC_OFF, tail)

    return bytes(buf)


def unpack_record(buf):
    """解包（不做校验，校验请用 slot_state）"""
    if len(buf) != REC_SIZE:
        raise ValueError("记录长度必须是 %d 字节，实际 %d" % (REC_SIZE, len(buf)))

    return {
        "seq": struct.unpack_from("<I", buf, 0)[0],
        "timestamp_s": struct.unpack_from("<I", buf, 4)[0],
        "temp_milli": struct.unpack_from("<i", buf, 8)[0],
        "humi_milli": struct.unpack_from("<I", buf, 12)[0],
        "press_pa": struct.unpack_from("<I", buf, 16)[0],
        "adc1": struct.unpack_from("<I", buf, 20)[0],
        "adc2": struct.unpack_from("<I", buf, 24)[0],
        "head_crc": struct.unpack_from("<H", buf, REC_HEAD_CRC_OFF)[0],
        "tail_crc": struct.unpack_from("<H", buf, REC_TAIL_CRC_OFF)[0],
    }


def slot_state(buf):
    """判定槽位状态，返回 SLOT_xxx。

    判定顺序与固件 recorder_slot_state() 完全一致，不能换：
    先判空 → 再判头 CRC → 最后判尾 CRC
    """
    if len(buf) != REC_SIZE:
        raise ValueError("记录长度必须是 %d 字节" % REC_SIZE)

    if all(b == 0xFF for b in buf):
        return SLOT_EMPTY

    head_calc = crc16_modbus(buf[0:REC_DATA_LEN])
    head_stored = struct.unpack_from("<H", buf, REC_HEAD_CRC_OFF)[0]
    if head_calc != head_stored:
        return SLOT_HALF

    tail_calc = crc16_ccitt(bytes(buf[0:REC_TAIL_CRC_OFF]))
    tail_stored = struct.unpack_from("<H", buf, REC_TAIL_CRC_OFF)[0]
    if tail_calc != tail_stored:
        return SLOT_UNFINISHED

    return SLOT_OK


# ============================================================
# 整数语义辅助（让 Python 严格模拟 C）
# ============================================================
def i32(x):
    """按 C 的 int32_t 截断（二进制补码）"""
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x >= 0x80000000 else x


def u32(x):
    """按 C 的 uint32_t 截断"""
    return x & 0xFFFFFFFF


def c_div(a, b):
    """C 的整数除法：向零截断（不是 Python 的向下取整）"""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


# ============================================================
# BME280 补偿算法（Bosch 官方整数版，与固件 bme280.c 逐位等价）
# ============================================================
CALIB_KEYS = ["T1", "T2", "T3", "P1", "P2", "P3", "P4", "P5", "P6",
              "P7", "P8", "P9", "H1", "H2", "H3", "H4", "H5", "H6"]


def compensate(adc_T, adc_P, adc_H, c):
    """完整补偿一遍，返回与固件相同的四个量。

    c 是 dict，键为 CALIB_KEYS（T1..H6）

    返回 dict：
        t_fine        温度补偿中间量（气压/湿度都依赖它）
        temp_centi    0.01 ℃
        press_q24_8   气压补偿原始返回（/256 = Pa）
        press_pa      Pa
        hum_q22_10    湿度补偿原始返回（/1024 = %RH）
        humi_centi    0.01 %RH
    """
    # ---------- 温度 ----------
    adc_T = i32(adc_T)
    var1 = i32(((adc_T >> 3) - i32(c["T1"] << 1)) * c["T2"]) >> 11
    var2 = i32((i32((adc_T >> 4) - c["T1"]) * i32((adc_T >> 4) - c["T1"])) >> 12)
    var2 = i32(var2 * c["T3"]) >> 14
    t_fine = i32(var1 + var2)
    temp_centi = i32(t_fine * 5 + 128) >> 8

    # ---------- 气压（必须 64 位中间量）----------
    v1 = t_fine - 128000
    v2 = v1 * v1 * c["P6"]
    v2 = v2 + ((v1 * c["P5"]) << 17)
    v2 = v2 + (c["P4"] << 35)
    v1 = ((v1 * v1 * c["P3"]) >> 8) + ((v1 * c["P2"]) << 12)
    v1 = (((1 << 47) + v1) * c["P1"]) >> 33

    if v1 == 0:
        press_q24_8 = 0
    else:
        p = 1048576 - i32(adc_P)
        p = c_div(((p << 31) - v2) * 3125, v1)
        w1 = (c["P9"] * (p >> 13) * (p >> 13)) >> 25
        w2 = (c["P8"] * p) >> 19
        p = ((p + w1 + w2) >> 8) + (c["P7"] << 4)
        press_q24_8 = u32(p)

    press_pa = press_q24_8 >> 8

    # ---------- 湿度 ----------
    v = i32(t_fine - 76800)
    left = i32(i32((i32(adc_H << 14) - i32(c["H4"] << 20) - i32(c["H5"] * v))
                   + 16384) >> 15)
    inner = i32(i32(i32((v * c["H6"]) >> 10)
                    * i32(i32((v * c["H3"]) >> 11) + 32768)) >> 10)
    inner = i32(inner + 2097152)
    inner = i32(i32(inner * c["H2"]) + 8192)
    right = i32(inner >> 14)
    v = i32(left * right)

    # ★★ 这里曾经有一个隐蔽的运算符优先级 bug，务必看清括号 ★★
    #
    # C 代码是：  v = v - ( ((((v>>15)*(v>>15)) >> 7) * dig_H1) >> 4 );
    #                                    ↑ 这个 >> 4 只作用于【减数】
    #
    # 但 Python 的运算符优先级里 `-` 比 `>>` 高，
    # 所以写成 `v - X >> 4` 实际算的是 `(v - X) >> 4` —— 完全不同的式子。
    # 表现：湿度算成 3.04 %RH（真值 55.48），差 18 倍，而温度/气压全对。
    #
    # 结论：**移位必须写在被除数自己的括号里**。
    corr = i32(i32(i32((v >> 15) * (v >> 15)) >> 7) * c["H1"])
    v = i32(v - i32(corr >> 4))

    if v < 0:
        v = 0
    if v > 419430400:
        v = 419430400

    hum_q22_10 = u32(v >> 12)
    humi_centi = u32((hum_q22_10 * 100) >> 10)

    return {
        "t_fine": t_fine,
        "temp_centi": temp_centi,
        "press_q24_8": press_q24_8,
        "press_pa": press_pa,
        "hum_q22_10": hum_q22_10,
        "humi_centi": humi_centi,
    }


# ============================================================
# 日志解析辅助（把串口抓下来的文本变成结构化数据）
# ============================================================
def parse_kv_lines(text, tag):
    """抽出所有 "#TAG,key=value,key=value" 行，返回 dict 列表。

    也容忍值里含 '=' 之前有空格（固件打印里有一处带中文说明）。
    """
    out = []
    prefix = "#" + tag + ","
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(prefix):
            continue
        body = line[len(prefix):]
        d = {}
        for part in body.split(","):
            if "=" in part:
                k, _, v = part.partition("=")
                d[k.strip()] = v.strip()
        out.append(d)
    return out


def parse_rec_lines(text):
    """抽出 "REC,<idx>,<hex>" 行，返回 [(idx, bytes), ...]"""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("REC,"):
            continue
        parts = line.split(",")
        if len(parts) < 3:
            continue
        try:
            idx = int(parts[1])
            raw = bytes.fromhex(parts[2])
        except ValueError:
            continue
        out.append((idx, raw))
    return out


def to_signed_16(x):
    x &= 0xFFFF
    return x - 0x10000 if x >= 0x8000 else x
