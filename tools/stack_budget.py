#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stack_budget.py —— 栈水位实测 → 重新分配 → 算压缩比

【这个脚本解决什么问题】
  简历项目经历第 1 条有一个红色占位符：
      用 uxTaskGetStackHighWaterMark 实测各任务栈水位并重新分配栈空间
      （压缩 ［实测后填］）

  这个「实测后填」必须填【你自己测出来的数】，不能猜。
  本脚本就是把它算出来的工具。

【FreeRTOS 的 uxTaskGetStackHighWaterMark 到底返回什么】
  ★ 它返回的是【历史最小剩余量】，单位是"字"（不是"已用"）。
  所以：
      峰值已用 = 分配栈大小 − 剩余水位
      推荐分配 = 峰值已用 × 1.5      ← 这是"峰值 ×1.5"的准确含义
  新手最容易在这里搞反：把"剩余"当成"已用"，
  于是算出来的栈只有一个零头，一跑就溢出。

【压缩比怎么算】
  要跟"经验值方案"比才有意义。本脚本默认的对照基准是
  "每个任务统一给 512 字"（就是那种拍脑袋的写法），
  压缩比 = 1 − 实测方案总栈 / 基准总栈

用法：
    python stack_budget.py log.txt
    python stack_budget.py log.txt --alloc acq=256,proc=256,comm=384
    python stack_budget.py log.txt --baseline 512
"""

import argparse
import sys
import os
import math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

WORD_BYTES = 4      # Cortex-M3 上 1 字 = 4 字节


def parse_stk_lines(text):
    """抽出所有 #STK,heap_free=...,tasks=...,acq=...,proc=...,comm=... 行"""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("#STK,"):
            continue
        d = {}
        for part in line.split(",")[1:]:
            if "=" in part:
                k, _, v = part.partition("=")
                try:
                    d[k.strip()] = int(v)
                except ValueError:
                    pass
        if {"acq", "proc", "comm"} <= set(d.keys()):
            out.append(d)
    return out


def main():
    ap = argparse.ArgumentParser(description="栈水位实测与重新分配计算")
    ap.add_argument("logfile", help="含 #STK,... 行的日志文件")
    ap.add_argument("--alloc", default="acq=256,proc=256,comm=384",
                    help="当前固件里各任务的分配值（字），默认 acq=256,proc=256,comm=384")
    ap.add_argument("--baseline", type=int, default=512,
                    help="对照基准：每个任务统一给多少字（默认 512）")
    args = ap.parse_args()

    alloc = {}
    for part in args.alloc.split(","):
        k, _, v = part.partition("=")
        alloc[k.strip()] = int(v)

    with open(args.logfile, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    lines = parse_stk_lines(text)
    if not lines:
        print("ERR: 没找到 #STK 行。")
        print("     固件每 60 秒会自动打一行，也可以给板子发命令 s 立即打印。")
        return 2

    print("解析到 %d 条 #STK 记录（取最小值作为最坏情况）" % len(lines))
    print()

    # 每个任务取"历史最小剩余"，即最坏情况
    worst = {}
    for task in ("acq", "proc", "comm"):
        worst[task] = min(d[task] for d in lines)

    heap_free = min(d.get("heap_free", 0) for d in lines)
    uart_hold = max(d.get("uart_hold_us", 0) for d in lines)
    lock_tmo = max(d.get("lock_tmo", 0) for d in lines)

    print("=" * 68)
    print("栈水位实测表（单位：字，1 字 = 4 字节）")
    print("=" * 68)
    print("  %-6s %-10s %-12s %-12s %-12s"
          % ("任务", "已分配", "最小剩余", "峰值已用", "推荐(峰值x1.5)"))
    print("  " + "-" * 64)

    total_alloc = 0
    total_recommend = 0
    detail = {}

    for task in ("acq", "proc", "comm"):
        a = alloc[task]
        left = worst[task]
        used = a - left
        if used < 0:
            print("  %-6s 数据异常：剩余(%d) > 分配(%d)，检查 --alloc 参数"
                  % (task, left, a))
            return 2
        rec = int(math.ceil(used * 1.5))
        # 兜个底：FreeRTOS 要求栈至少能装下上下文，太小会立刻溢出
        if rec < 64:
            rec = 64
        detail[task] = (a, left, used, rec)
        total_alloc += a
        total_recommend += rec
        print("  %-6s %-10d %-12d %-12d %-12d" % (task, a, left, used, rec))

    print("  " + "-" * 64)
    print("  %-6s %-10d %-12s %-12s %-12d"
          % ("合计", total_alloc, "", "", total_recommend))

    print()
    print("=" * 68)
    print("压缩比")
    print("=" * 68)

    base_total = args.baseline * 3
    print("  当前（宽裕档）总栈        : %d 字 = %d 字节"
          % (total_alloc, total_alloc * WORD_BYTES))
    print("  实测重分配后总栈          : %d 字 = %d 字节"
          % (total_recommend, total_recommend * WORD_BYTES))
    print("  对照基准（每任务 %d 字）: %d 字 = %d 字节"
          % (args.baseline, base_total, base_total * WORD_BYTES))
    print()

    cut_vs_base = (1.0 - float(total_recommend) / float(base_total)) * 100.0
    cut_vs_alloc = (1.0 - float(total_recommend) / float(total_alloc)) * 100.0

    print("  相对「统一 %d 字」基准，压缩 : %.1f%%" % (args.baseline, cut_vs_base))
    print("  相对当前宽裕档，压缩         : %.1f%%" % cut_vs_alloc)
    print()

    # ---------- ★ 可信度前置检查：采样数 与 "深路径是否跑过" ----------
    #   uxTaskGetStackHighWaterMark 返回的是【历史最小剩余】，
    #   所以它只能反映【已经跑过的那些代码路径】。
    #   深路径（自检 t / 倒出 r）还没跑的话，这个"峰值"就是偏乐观的 ——
    #   照它去缩栈，一跑深路径就可能溢出。
    n_stk = len(lines)
    deep_marks = []
    for tag, desc in (("#REC", "命令 t（记录闭环）"),
                      ("#SPI4", "命令 t（SPI 四模式）"),
                      ("#PWR", "命令 t（断电三情况）"),
                      ("#DUMP", "命令 r（记录倒出）")):
        if tag in text:
            deep_marks.append(desc)

    # ★ 收尾脚本现在是【先倒出、后采样】——
    #   所以只要日志里有 "(finish, after dump)" 这个标记，
    #   就说明最后那次采样发生在倒出（r，comm 最深调用链之一）之后，
    #   深路径已经被覆盖，不必再要求日志里出现 #DUMP 痕迹。
    if "(finish, after dump)" in text:
        if not deep_marks:
            deep_marks.append("命令 r（记录倒出，收尾脚本在倒出之后采样）")

    if (n_stk < 10) or (not deep_marks):
        print("=" * 68)
        print("⚠️  先别把这个数字填进简历 —— 可信度还不够")
        print("=" * 68)
        print("  本次只解析到 %d 条 #STK 采样。" % n_stk)
        if not deep_marks:
            print("  而且日志里【看不到深路径的痕迹】：")
            print("    · 命令 t（W4 全量自检）—— comm 任务里最深的调用链")
            print("    · 命令 r（记录倒出）")
        else:
            print("  已看到的深路径痕迹：%s" % "、".join(sorted(set(deep_marks))))
        print()
        print("  为什么这很重要：")
        print("    uxTaskGetStackHighWaterMark 给的是【历史最小剩余】，")
        print("    它只能反映【已经跑过的代码路径】。")
        print("    上面的剩余水位只是「日常路径」的水位 ——")
        print("    照它把 comm 栈缩到推荐值，一跑 t 就可能溢出，")
        print("    症状是莫名其妙的 HardFault 或数据错乱。")
        print()
        print("  正确顺序：")
        print("    ① 先跑命令 t（需短接 PA7-PA6）、再跑 r，把深路径都走到")
        print("    ② 让板子连续跑几小时以上（推荐 24 h，APP_UART_VERBOSE=0）")
        print("    ③ 再跑本脚本 —— 那时得到的才是能写进简历的数字")
        print()

    # ---------- 直接给出可以粘进简历的数字 ----------
    print("=" * 68)
    print("可以直接填进简历红色占位符的结论")
    print("=" * 68)
    print("  用 uxTaskGetStackHighWaterMark 实测各任务栈水位并重新分配栈空间")
    print("  （压缩 %.0f%%）—— 相对「每任务统一 %d 字」的经验值方案，"
          % (cut_vs_base, args.baseline))
    print("  总栈从 %d 字降至 %d 字（%d B → %d B）。"
          % (base_total, total_recommend,
             base_total * WORD_BYTES, total_recommend * WORD_BYTES))
    print()
    print("  写进终端或论文前请核对三点：")
    print("    ① --alloc 必须与你当时烧录的固件一致（横幅里 #BUILD,APP_STACK_MEASURED 可佐证）")
    print("    ② 压缩比要说明是「相对哪个基准」，否则面试官会追问到底")
    print("    ③ 填完之后，把 APP_STACK_MEASURED 改成 1 并按推荐值重新编译跑一遍，")
    print("       确认新的 #STK 里剩余水位仍然 > 0（有余量才算真的安全）")

    # ---------- 其他诊断 ----------
    print()
    print("其他诊断：")
    print("  堆最小剩余           : %d 字节" % heap_free)
    if heap_free < 1024:
        print("    ⚠ 低于 1 KB，建议调大 configTOTAL_HEAP_SIZE 或减小任务栈")
    print("  UART 锁最长持有      : %d µs" % uart_hold)
    if uart_hold > 20000:
        print("    ⚠ 超过 20 ms，说明有人在持锁期间做了大量串口输出 —— "
              "会把等锁的任务饿住")
    print("  取锁超时次数         : %d" % lock_tmo)
    if lock_tmo > 0:
        print("    ⚠ 出现了取锁超时，说明某个任务持锁时间过长，值得查一下")

    print()
    print("推荐的新栈尺寸（写进 app_tasks.c 的 APP_STACK_MEASURED 分支）：")
    print("      #define STK_ACQ     %dU" % detail["acq"][3])
    print("      #define STK_PROC    %dU" % detail["proc"][3])
    print("      #define STK_COMM    %dU" % detail["comm"][3])

    return 0


if __name__ == "__main__":
    sys.exit(main())
