#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
golden_vectors.py —— 用【真实板子实测数据】做基准向量，防止 multibus.py 被改坏

【为什么需要这个文件 —— 一次真实翻车教训】

  之前"PC 端工具链已验证通过（BME280 复算 18 项 0 不一致）"这句话是【假 PASS】。
  原因：那份假日志是 make_sample_log.py 用 multibus.py 算出来的，
  而验证时又用 multibus.py 去复算它 —— 同一条有 bug 的代码自己对自己，
  必然自洽，必然报一致。

  【用被测代码自己造测试数据，等于没测。】

  然后用真板子跑，立刻暴露：湿度算成 3.04 %RH，而固件给的是 55.48 %RH。
  根因是 multibus.py 里 `v - X >> 4` 的 Python 运算符优先级问题
  （Python 的 `-` 比 `>>` 高，C 里不是）。

  所以本文件收录【固件那侧独立算出来的】输入与输出。
  固件的补偿是用 C 逐字照 Bosch 公式写的，与 Python 实现是两套独立代码，
  因此这组数据是真正的外部基准，不是自我循环。

【用法】
  python golden_vectors.py            # 跑基准测试
  python golden_vectors.py --selftest # 同上（给脚本调用）

【改动 multibus.py 之后必须跑一次】
"""

import sys

import multibus

# ============================================================
# 基准向量：2026-09-17 首次上板实测（STM32F103C8T6 + BME280）
#
# 数据来源：板子串口输出，未经任何人工修改。
# 期望值全部由【固件】给出（C 实现），所以能独立校验 Python 实现。
#
# 固件命令：
#   c → #CALIB（33 个校准系数）
#   m → #RAW / #CMP / #OUT
# 抓取脚本：serial_capture.py --port COM8 --seq c,m,m,m,m,m --gap 2
# 原始日志：firmware/logs/bme.txt
# ============================================================

# 33 个出厂校准系数（#CALIB）
CALIB = {
    "T1": 28399, "T2": 26637, "T3": 50,
    "P1": 37164, "P2": -10691, "P3": 3024, "P4": 6764, "P5": 30,
    "P6": -7, "P7": 11700, "P8": -11800, "P9": 5000,
    "H1": 75, "H2": 368, "H3": 0, "H4": 305, "H5": 50, "H6": 30,
}

# 5 个样本：(原始 ADC, t_fine, 固件的期望输出)
#
# 每个 tuple = (adc_T, adc_P, adc_H, t_fine,
#               temp_centi, press_q24_8, press_pa, hum_q22_10, humi_centi)
SAMPLES = [
    (538907, 341041, 29403, 137432, 2684, 25862231, 101024, 56817, 5548),
    (538896, 341034, 29406, 137419, 2684, 25862431, 101025, 56839, 5550),
    (538887, 341030, 29411, 137393, 2683, 25862396, 101024, 56862, 5552),
    (538886, 341031, 29409, 137393, 2683, 25862352, 101024, 56850, 5551),
    (538884, 341027, 29423, 137393, 2683, 25862527, 101025, 56932, 5559),
]

# 逐项字段：(结果字典里的键, 在 SAMPLES 里的下标, 中文说明)
FIELDS = [
    ("t_fine",     3, "t_fine（温度补偿中间量）"),
    ("temp_centi", 4, "温度 0.01℃"),
    ("press_q24_8", 5, "气压补偿原始返回 Q24.8"),
    ("press_pa",   6, "气压 Pa"),
    ("hum_q22_10", 7, "湿度补偿原始返回 Q22.10"),
    ("humi_centi", 8, "湿度 0.01%RH"),
]


# ============================================================
# 第二组基准：记录头尾 CRC 的【反退化】检查
#
# 2026-09-17 实测翻车：
#   尾 CRC 原本写成"从头 CRC 的状态续算最后两个字节"。
#   CRC-16/MODBUS 满足【追加性质】——把 CRC 追加到数据后再算一次，结果恒为 0。
#   ⇒ 尾 CRC 恒等于 0x0000，与数据无关，一条信息的校验价值都没有。
#   实测：1246 条记录里 1245 条的尾 CRC 存值都是 0x0000。
#
# 下面这组断言就是用来防止这个退化被改回来的。
# ============================================================

def check_record_crc():
    print("=" * 66)
    print("记录 CRC 反退化检查（防止尾 CRC 退化成常数）")
    print("=" * 66)
    print()

    fails = []

    # 造 6 条内容不同的记录
    recs = []
    for i in range(6):
        recs.append(multibus.pack_record(
            seq=i,
            timestamp_s=1000 + i,
            temp_milli=26560 + i * 7,
            humi_milli=56010 + i * 31,
            press_pa=101020 + i,
        ))

    heads = [multibus.unpack_record(r)["head_crc"] for r in recs]
    tails = [multibus.unpack_record(r)["tail_crc"] for r in recs]

    print("  6 条记录（仅 seq/时间戳/温湿压略有差异）：")
    for i, r in enumerate(recs):
        u = multibus.unpack_record(r)
        print("    #%d  头CRC=0x%04X  尾CRC=0x%04X  slot=%s"
              % (i, u["head_crc"], u["tail_crc"],
                 multibus.SLOT_NAME[multibus.slot_state(r)]))
    print()

    # ① 打包出来的记录必须判定为完整
    for i, r in enumerate(recs):
        if multibus.slot_state(r) != multibus.SLOT_OK:
            fails.append("记录 #%d 打包后不是 SLOT_OK" % i)
    print("  ① 打包后都判为完整(OK)          : %s"
          % ("OK" if not fails else "*** 失败 ***"))

    # ② 头 CRC 必须随数据变化
    n1 = len(set(heads))
    ok_head = (n1 == len(recs))
    if not ok_head:
        fails.append("头 CRC 出现了重复（%d 个不同值 / %d 条）" % (n1, len(recs)))
    print("  ② 头 CRC 互不相同（%d/%d）      : %s"
          % (n1, len(recs), "OK" if ok_head else "*** 退化 ***"))

    # ③ ★ 尾 CRC 必须随数据变化 —— 这正是旧实现失败的那一条
    n2 = len(set(tails))
    ok_tail = (n2 == len(recs))
    if not ok_tail:
        fails.append("尾 CRC 出现了重复（%d 个不同值 / %d 条）—— 退化成常数了"
                     % (n2, len(recs)))
    print("  ③ 尾 CRC 互不相同（%d/%d）      : %s"
          % (n2, len(recs), "OK" if ok_tail else "*** 退化 ***"))

    # ④ 尾 CRC 不能是"同多项式的任何一种退化形式"
    #    退化形式 A：从头 CRC 续算（MODBUS，旧实现）→ 恒 0x0000
    #    退化形式 B：MODBUS 换 init=0x0000 重算 0–29 → 恒 0x4FFE
    #    两者都已实测验证过，这里用来防止被改回去。
    degenerate = 0
    for r in recs:
        u = multibus.unpack_record(r)
        form_a = multibus.crc16_modbus(
            bytes(r[multibus.REC_HEAD_CRC_OFF:multibus.REC_HEAD_CRC_OFF + 2]),
            u["head_crc"])
        form_b = multibus.crc16_modbus_init(0x0000, bytes(r[0:multibus.REC_TAIL_CRC_OFF]))
        if u["tail_crc"] in (form_a, form_b):
            degenerate += 1
    ok_notcont = (degenerate == 0)
    if not ok_notcont:
        fails.append("有 %d 条记录的尾 CRC 落在同多项式的退化形式上（已退化）"
                     % degenerate)
    print("  ④ 尾 CRC 非同多项式退化形式    : %s"
          % ("OK" if ok_notcont else "*** 退化 ***"))

    # ⑦ CCITT 实现本身的正确性（公认检查值）
    got = multibus.crc16_ccitt(b"123456789")
    ok_cc = (got == multibus.CRC16_CCITT_CHECK)
    if not ok_cc:
        fails.append("crc16_ccitt('123456789') = 0x%04X，应为 0x%04X"
                     % (got, multibus.CRC16_CCITT_CHECK))
    print("  ⑦ CCITT 标准检查值 0x%04X      : %s"
          % (multibus.CRC16_CCITT_CHECK, "OK" if ok_cc else "*** 失败 ***"))

    # ⑤ 破坏尾 CRC → 必须判为"未收尾"，而不是"半写"
    b = bytearray(recs[0])
    b[30] ^= 0xFF
    st = multibus.slot_state(bytes(b))
    ok_unf = (st == multibus.SLOT_UNFINISHED)
    if not ok_unf:
        fails.append("破坏尾 CRC 后判定为 %s，应为 SLOT_UNFINISHED"
                     % multibus.SLOT_NAME[st])
    print("  ⑤ 破坏尾 CRC → 判为未收尾      : %s" % ("OK" if ok_unf else "*** 失败 ***"))

    # ⑥ 破坏数据区 → 必须判为"半写"
    b2 = bytearray(recs[0])
    b2[0] ^= 0xFF
    st2 = multibus.slot_state(bytes(b2))
    ok_half = (st2 == multibus.SLOT_HALF)
    if not ok_half:
        fails.append("破坏数据区后判定为 %s，应为 SLOT_HALF" % multibus.SLOT_NAME[st2])
    print("  ⑥ 破坏数据区 → 判为半写        : %s" % ("OK" if ok_half else "*** 失败 ***"))

    print()
    if fails:
        print("  失败项：")
        for f in fails:
            print("    - " + f)
    print("=" * 66)
    return fails


def main():
    print("=" * 66)
    print("基准向量测试：用真板子实测数据校验 multibus.py")
    print("=" * 66)
    print()
    print("数据来源：2026-09-17 STM32F103C8T6 + BME280 首次上板实测")
    print("          期望值由【固件 C 实现】给出（与 Python 是两套独立代码）")
    print("          %d 个样本 × %d 个量 = %d 项比对"
          % (len(SAMPLES), len(FIELDS), len(SAMPLES) * len(FIELDS)))
    print()

    total = 0
    bad = 0

    for i, row in enumerate(SAMPLES):
        adc_T, adc_P, adc_H, exp_t_fine, exp_tc, exp_p, exp_pp, exp_hq, exp_hc = row
        got = multibus.compensate(adc_T, adc_P, adc_H, CALIB)

        print("样本 %d: adc_T=%d adc_P=%d adc_H=%d" % (i, adc_T, adc_P, adc_H))
        for key, idx, label in FIELDS:
            expect = row[idx]
            actual = got[key]
            total += 1
            ok = (actual == expect)
            if not ok:
                bad += 1
            print("   %-30s 固件=%-10d PC=%-10d %s"
                  % (label, expect, actual, "OK" if ok else "*** 不一致 ***"))
        print()

    print("=" * 66)
    print("共 %d 项，不一致 %d 项" % (total, bad))
    if bad == 0:
        print("结论：PASS —— multibus.py 的补偿算法与固件【逐位一致】")
        print()
        print("★ 这一条才是可信的验证：因为期望值来自固件（C），")
        print("  而不是来自 multibus.py 自己生成的假日志。")
    else:
        print("结论：FAIL —— multibus.py 被改坏了，或固件改了补偿算法。")
        print()
        print("排查顺序（都是这次真踩过的坑）：")
        print("  ① 只看湿度不一致 → 查 `v - X >> 4` 这类【运算符优先级】")
        print("      Python 的 `-` 比 `>>` 高优先级，C 里不是。")
        print("      移位必须写在被除数自己的括号里：v - (X >> 4)")
        print("  ② t_fine 就不一致 → 温度补偿的整数运算有差异")
        print("      （查 C 的 >> 对负数是算术右移，以及 int32 溢出回绕）")
        print("  ③ 气压不一致 → 除法语义（C 向零截断，Python // 向下取整）")
        print("      本文件用 multibus.c_div()，已按 C 语义实现")
    print("=" * 66)

    # ---------- 第二组：记录 CRC 反退化 ----------
    print()
    crc_fails = check_record_crc()

    if bad == 0 and not crc_fails:
        print()
        print("全部通过：补偿算法逐位一致 + 记录 CRC 未退化")
        return 0

    print()
    print("结论：有失败项（见上）。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
