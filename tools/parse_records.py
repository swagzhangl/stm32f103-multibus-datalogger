#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
parse_records.py —— 32 字节记录流逐条校验（头尾双 CRC）

【这一步在证明什么】
  简历上写："设计 32 字节定长记录格式（含头尾双 CRC-16/MODBUS），
              打通 I2C 采集 → SPI 存储 → UART 上传 → PC 逐字节校验的完整闭环"

  这个脚本是那个"闭环"的最后一环：
    板子把 Flash 里的记录按 HEX 吐出来（串口命令 r），
    PC 端【逐字节】重新算两遍 CRC 并与记录里存的值比对。

  ★ 为什么必须是"逐字节重新算"而不是"看板子报没报错"：
    板子自己报 OK 只说明它自己前后一致；
    只有让一个【独立实现】拿到原始字节重算，才能证明
    "从传感器到 Flash 到串口到 PC"整条链路上的字节没有一个被改过。

用法：
    python parse_records.py log.txt
    python parse_records.py log.txt --csv records.csv
    python parse_records.py log.txt --show 10
"""

import argparse
import sys
import os
import csv

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from multibus import (REC_SIZE, SLOT_OK, SLOT_HALF, SLOT_UNFINISHED, SLOT_EMPTY,
                      SLOT_NAME, crc16_modbus, crc16_selftest,
                      unpack_record, slot_state, parse_rec_lines)


def main():
    ap = argparse.ArgumentParser(description="解析并校验 32 B 记录流")
    ap.add_argument("logfile", help="含 REC,<idx>,<hex> 行的日志文件")
    ap.add_argument("--csv", help="把通过校验的记录导出为 CSV")
    ap.add_argument("--show", type=int, default=0, help="打印前 N 条记录内容")
    args = ap.parse_args()

    ok, got = crc16_selftest()
    print("CRC-16/MODBUS 自检: %s (0x%04X)" % ("PASS" if ok else "FAIL", got))
    if not ok:
        print("ERR: CRC 实现自检失败，后续校验无意义")
        return 2

    with open(args.logfile, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    recs = parse_rec_lines(text)
    if not recs:
        print()
        print("ERR: 没找到 REC,<idx>,<hex> 行。")
        print("     请先给板子发命令 r（把已存记录按 HEX 吐出来）。")
        return 2

    print("解析到 %d 条记录行" % len(recs))
    print()

    # ---------- 逐条校验 ----------
    stat = {SLOT_EMPTY: 0, SLOT_OK: 0, SLOT_HALF: 0, SLOT_UNFINISHED: 0}
    bad_len = []
    bad_head = []
    bad_tail = []
    header_crc_ok_tail_bad = []      # 关键：数据完整但未收尾
    rows = []

    for idx, raw in recs:
        if len(raw) != REC_SIZE:
            bad_len.append((idx, len(raw)))
            continue

        st = slot_state(raw)
        stat[st] += 1

        if st == SLOT_HALF:
            bad_head.append(idx)
        elif st == SLOT_UNFINISHED:
            bad_tail.append(idx)
            header_crc_ok_tail_bad.append(idx)

        if st == SLOT_OK:
            d = unpack_record(raw)
            rows.append((idx, d))

    # ---------- 序号连续性检查 ----------
    # 定长记录 + 序号连续，是"单 CRC 方案"用来补偿丢记录检测的手段；
    # 32 B 方案有双 CRC 了，但序号连续性依然是"有没有整条丢失"的独立证据。
    gaps = []
    for i in range(1, len(rows)):
        prev = rows[i - 1][1]["seq"]
        cur = rows[i][1]["seq"]
        if cur != (prev + 1) & 0xFFFFFFFF:
            gaps.append((rows[i - 1][0], rows[i][0], prev, cur))

    # ---------- 汇总 ----------
    print("=" * 62)
    print("校验结果")
    print("=" * 62)
    print("  有效记录 (头尾 CRC 都通过) : %d" % stat[SLOT_OK])
    print("  半写记录 (头 CRC 失败)      : %d" % stat[SLOT_HALF])
    print("  未收尾   (头过、尾失败)     : %d" % stat[SLOT_UNFINISHED])
    print("  空槽     (全 0xFF)          : %d" % stat[SLOT_EMPTY])
    if bad_len:
        print("  长度异常                    : %d  (前几个: %s)"
              % (len(bad_len), bad_len[:5]))

    if bad_head:
        print()
        print("  头 CRC 失败的记录下标(前 10): %s" % bad_head[:10])
    if header_crc_ok_tail_bad:
        print()
        print("  ★ 头 CRC 通过但尾 CRC 失败的下标(前 10): %s"
              % header_crc_ok_tail_bad[:10])
        print("    这正是 32 B 头尾双 CRC 的价值所在：")
        print("      它说明【数据区完好，只是这条记录没写完】——")
        print("      单 CRC 方案只能报「整条坏了」，会把这份好数据一起丢掉。")
        print("      它出现的时机是：两次页编程（先 30 字节、再 2 字节）之间断电。")

    print()
    if gaps:
        print("  序号不连续 %d 处（前 5）:" % len(gaps))
        for a, b, pa, cu in gaps[:5]:
            print("    记录下标 %d -> %d : seq %d -> %d" % (a, b, pa, cu))
    else:
        print("  序号连续性: 全部连续，无整条丢失")

    print()
    if stat[SLOT_HALF] == 0 and stat[SLOT_UNFINISHED] == 0 and not bad_len:
        print("结论: PASS —— 全部 %d 条记录的头、尾 CRC 均在 PC 端独立重算通过"
              % stat[SLOT_OK])
        print()
        print("可以直接写进简历/论文的话：")
        print("  「打通 I2C 采集 → SPI 存储 → UART 上传 → PC 逐字节校验的完整闭环」")
        print("  证据：%d 条 × 32 字节 = %d 字节，在 PC 端逐条重算"
              % (stat[SLOT_OK], stat[SLOT_OK] * REC_SIZE))
        print("        头 CRC（覆盖 0–27）与尾 CRC（覆盖 0–29）全部一致。")
    else:
        print("结论: 有异常记录 —— 见上面的分类。")
        print("      若有 HALF/UNFINISHED，可能是断电测试留下的（属正常现象）；")
        print("      若是大量 HALF，重点查扇区擦除与页编程时序。")

    # ---------- 打印前 N 条 ----------
    if args.show > 0:
        print()
        print("前 %d 条记录内容：" % min(args.show, len(rows)))
        print("  %-6s %-10s %-12s %-12s %-12s" %
              ("下标", "seq", "时间戳(s)", "温度(℃)", "湿度(%RH)"))
        for idx, d in rows[:args.show]:
            print("  %-6d %-10d %-12d %-12.3f %-12.3f"
                  % (idx, d["seq"], d["timestamp_s"],
                     d["temp_milli"] / 1000.0, d["humi_milli"] / 1000.0))

    # ---------- 导出 CSV ----------
    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f)
            w.writerow(["index", "seq", "timestamp_s", "temp_c", "humi_pct",
                        "press_pa", "press_hpa", "adc1", "adc2",
                        "head_crc_hex", "tail_crc_hex", "state"])
            for idx, raw in recs:
                if len(raw) != REC_SIZE:
                    continue
                st = slot_state(raw)
                d = unpack_record(raw)
                w.writerow([idx, d["seq"], d["timestamp_s"],
                            d["temp_milli"] / 1000.0,
                            d["humi_milli"] / 1000.0,
                            d["press_pa"], d["press_pa"] / 100.0,
                            d["adc1"], d["adc2"],
                            "0x%04X" % d["head_crc"], "0x%04X" % d["tail_crc"],
                            SLOT_NAME.get(st, "?")])
        print()
        print("已导出 CSV: %s（UTF-8 BOM，Excel 可直接打开）" % args.csv)

    return 0 if (stat[SLOT_HALF] == 0 and stat[SLOT_UNFINISHED] == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
