#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_bme280.py —— BME280 温湿压"PC 端复算逐位一致"验证

【这一步在证明什么】
  简历上写"温湿压结果经 PC 端复算逐位一致"。
  这个脚本就是那句话的验证器：

    固件通过串口给出  #CALIB（33 个出厂校准系数）
                      #RAW  （3 个 20/16 位原始 ADC 值 + t_fine）
                      #CMP  （两个补偿函数的原始返回）
                      #OUT  （最终物理量）

    PC 端【只用 #CALIB 和 #RAW 里的 adc 值】，
    用自己的 Python 实现重新算一遍，
    然后把结果和 #CMP / #OUT 逐位比对。

  ★ 关键在"只用输入"：如果拿固件的中间量再去算最终值，
    那叫"抄答案"，证明不了任何事。

用法：
    python verify_bme280.py log.txt
    python verify_bme280.py log.txt --verbose

怎么拿到 log.txt：
    python serial_capture.py --port COM5 --baud 115200 --out log.txt --seconds 20
    期间在串口助手/工具里发 c 和若干次 m 命令
    （或直接开机自检 APP_SELFTEST_ON_BOOT=1）
"""

import argparse
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from multibus import CALIB_KEYS, compensate, crc16_selftest


def parse_log(text):
    """按行扫描，把 #CALIB / #RAW / #CMP / #OUT 组织成样本列表。

    用状态机而不是"按 tag 分组后 zip"：因为可能出现
    #RAW,ERR,... 这种失败行，按 tag 分组会把它们和成功样本错位对齐，
    导致"明明算对了却报不一致"。
    """
    calib = None
    samples = []
    cur = {}
    bad = 0

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line.startswith("#"):
            continue

        tag = line[1:].split(",", 1)[0].upper()

        if tag == "CRC":
            continue

        if tag == "CALIB":
            if ",ERR" in line:
                continue
            d = {}
            for part in line.split(",")[1:]:
                if "=" in part:
                    k, _, v = part.partition("=")
                    k = k.strip()
                    if k in CALIB_KEYS:
                        try:
                            d[k] = int(v)
                        except ValueError:
                            pass
            if len(d) == 18:
                calib = d
            continue

        if tag == "RAW":
            if ",ERR" in line:
                bad += 1
                cur = {}
                continue
            cur = {}
            for part in line.split(",")[1:]:
                if "=" in part:
                    k, _, v = part.partition("=")
                    try:
                        cur[k.strip()] = int(v)
                    except ValueError:
                        pass
            continue

        if tag == "CMP":
            for part in line.split(",")[1:]:
                if "=" in part:
                    k, _, v = part.partition("=")
                    try:
                        cur[k.strip()] = int(v)
                    except ValueError:
                        pass
            continue

        if tag == "OUT":
            for part in line.split(",")[1:]:
                if "=" in part:
                    k, _, v = part.partition("=")
                    try:
                        cur[k.strip()] = int(v)
                    except ValueError:
                        pass
            if all(k in cur for k in
                   ("adc_T", "adc_P", "adc_H",
                    "press_q24_8", "hum_q22_10",
                    "temp_centi", "humi_centi", "press_pa")):
                samples.append(dict(cur))
            cur = {}
            continue

    return calib, samples, bad


def main():
    ap = argparse.ArgumentParser(description="BME280 PC 端复算验证")
    ap.add_argument("logfile", help="串口抓取的日志文件")
    ap.add_argument("--verbose", action="store_true", help="打印每个样本的比对明细")
    args = ap.parse_args()

    # 先自检 CRC：两边算法不一致的话，后面都不用看了
    ok, got = crc16_selftest()
    print("CRC-16/MODBUS 自检: %s (0x%04X)" % ("PASS" if ok else "FAIL", got))

    with open(args.logfile, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    calib, samples, bad = parse_log(text)

    if calib is None:
        print()
        print("ERR: 日志里没有找到完整的 #CALIB 行。")
        print("     请先给板子发命令 c（打印 33 个校准系数），再发若干次 m。")
        return 2

    if not samples:
        print()
        print("ERR: 日志里没有找到配对完整的 #RAW/#CMP/#OUT 样本。")
        print("     请给板子发命令 m（触发一次测量并打印原始量）。")
        if bad:
            print("     （另外发现 %d 条 #RAW,ERR 行，说明取传感器失败）" % bad)
        return 2

    print("解析到校准系数 18 项（覆盖全部 33 个：其中 H4/H5 是 12 位拼接值）")
    print("解析到测量样本 %d 个" % len(samples))
    if bad:
        print("另有 %d 条 #RAW,ERR（取数失败）" % bad)
    print()

    # ---------- 逐样本复算 ----------
    fields = [
        ("t_fine", "t_fine", "温度补偿中间量"),
        ("temp_centi", "temp_centi", "温度 0.01℃"),
        ("press_q24_8", "press_q24_8", "气压补偿原始返回 Q24.8"),
        ("press_pa", "press_pa", "气压 Pa"),
        ("hum_q22_10", "hum_q22_10", "湿度补偿原始返回 Q22.10"),
        ("humi_centi", "humi_centi", "湿度 0.01%RH"),
    ]

    total_cmp = 0
    total_bad = 0
    first_bad_report = []

    for si, s in enumerate(samples):
        mine = compensate(s["adc_T"], s["adc_P"], s["adc_H"], calib)

        line_bad = []
        for key, mkey, label in fields:
            if key not in s:
                continue
            total_cmp += 1
            if mine[mkey] != s[key]:
                total_bad += 1
                line_bad.append((label, s[key], mine[mkey]))

        if line_bad:
            if len(first_bad_report) < 3:
                first_bad_report.append((si, s, mine, line_bad))
        elif args.verbose:
            print("样本 %-3d adc_T=%-9d adc_P=%-9d adc_H=%-6d  ->  "
                  "T=%.2f℃  H=%.2f%%RH  P=%.2f hPa   全部一致"
                  % (si, s["adc_T"], s["adc_P"], s["adc_H"],
                     mine["temp_centi"] / 100.0,
                     mine["humi_centi"] / 100.0,
                     mine["press_pa"] / 100.0))

    # ---------- 结论 ----------
    print()
    print("=" * 60)
    print("复算比对：共 %d 项，其中不一致 %d 项" % (total_cmp, total_bad))

    if total_bad == 0:
        print("结论：PASS —— PC 端独立复算与固件输出【逐位一致】")
        print()
        print("可以直接写进简历/论文的话：")
        print("  「温湿压结果经 PC 端独立复算逐位一致」")
        print("  证据：%d 个样本 × 6 个量 = %d 项比对全部相同，"
              % (len(samples), total_cmp))
        print("        且 PC 端只使用原始 ADC 值与出厂校准系数，未复用固件中间结果。")
    else:
        print("结论：FAIL")
        print()
        for si, s, mine, line_bad in first_bad_report:
            print("样本 %d:" % si)
            print("  adc_T=%d adc_P=%d adc_H=%d" % (s["adc_T"], s["adc_P"], s["adc_H"]))
            for label, board_v, pc_v in line_bad:
                print("  %-28s 固件=%-12d PC=%-12d 差=%d"
                      % (label, board_v, pc_v, board_v - pc_v))
        print()
        print("排查方向：")
        print("  ① 若 t_fine 就不一致 → 温度补偿的整数运算有差异")
        print("     （重点查 C 的 >> 对负数是算术右移，以及 int32 溢出回绕）")
        print("  ② 若 t_fine 对但气压不一致 → 气压补偿的除法语义")
        print("     （C 是向零截断，Python // 是向下取整；本脚本已按 C 语义实现）")
        print("  ③ 若只有湿度不一致 → 查 H4/H5 的 12 位跨字节拼接")

    return 0 if total_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
