#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run.py —— 傻瓜式菜单（给不想学 Python 的人用）

【这是什么】
  把 tools/ 里那几个脚本包成一个菜单。你只要：
      ① 敲一条命令启动它
      ② 选数字、回车
  不用记任何参数。

【怎么启动】
  打开固件工程文件夹（firmware），在地址栏输入 cmd 回车，然后敲：
      python tools\\run.py

  或者已经在 tools 文件夹里：
      python run.py

【它不会替你决定什么】
  每一步跑完，它会把"该看哪一行""什么算通过"打出来 ——
  判断还是你做。因为这些结论要写进简历，必须你自己确认过。
"""

import glob
import os
import shutil
import sys
import subprocess
import time

# ---------- 路径：以本文件所在目录为基准，从哪运行都一样 ----------
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.dirname(TOOLS_DIR)
LOG_DIR = os.path.join(FW_DIR, "logs")
PORT_FILE = os.path.join(TOOLS_DIR, ".last_port")
CLI_FILE = os.path.join(TOOLS_DIR, ".cli_path")

# ---------- CubeProgrammer 可执行文件可能在哪 ----------
#   不写死盘符 / 版本号，保证换一台机器、换个安装位置也能找到。
#   优先级：环境变量 STM32_CUBEPROGRAMMER → PATH → CubeIDE 插件目录 → 独立安装版
CLI_ENV_VAR = "STM32_CUBEPROGRAMMER"

# CubeIDE 把 CubeProgrammer 作为插件装在自己目录里，插件目录名带版本号，
# 所以用通配符匹配 —— 升级 CubeIDE 后版本号变了照样能找到。
_CLI_REL = os.path.join(
    "*STM32CubeIDE*", "STM32CubeIDE", "plugins",
    "com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*",
    "tools", "bin", "STM32_Programmer_CLI.exe")


def _drives():
    """当前存在的盘符（Windows）；非 Windows 返回 ['']。"""
    if os.name != "nt":
        return [""]
    return [c + ":" for c in "CDEFGHIJKLMNOPQRSTUVWXYZ"
            if os.path.exists(c + ":\\")]


def cli_patterns():
    """生成候选路径模式（按优先级）。"""
    pats = []

    # ① 显式指定：整个路径直接给。
    #    set STM32_CUBEPROGRAMMER=D:\somewhere\STM32_Programmer_CLI.exe
    env = os.environ.get(CLI_ENV_VAR, "").strip()
    if env:
        pats.append(env)

    # ② CubeIDE 插件目录：盘符 × 常见安装前缀 × 通配版本号
    for d in _drives():
        for prefix in ("", "ST",
                       r"Program Files\STMicroelectronics",
                       r"Program Files (x86)\STMicroelectronics"):
            pats.append(os.path.join(d + os.sep, prefix, _CLI_REL))

    # ③ 单独安装的 STM32CubeProgrammer（GUI 版自带 CLI）
    for d in _drives():
        for pf in (r"Program Files\STMicroelectronics",
                   r"Program Files (x86)\STMicroelectronics"):
            pats.append(os.path.join(
                d + os.sep, pf, "STM32Cube", "STM32CubeProgrammer",
                "bin", "STM32_Programmer_CLI.exe"))

    return pats

SEP = "=" * 64
LINE = "-" * 64


# ============================================================
# 小工具
# ============================================================
def clear():
    """清屏。

    ★ 输出被重定向时【不清屏】——
      因为清屏会把已经打出来的内容冲掉，而"把输出存成文件当证据"是常用做法。
      只有直接对着控制台运行时才清屏。"""
    if not sys.stdout.isatty():
        print()
        return
    os.system("cls" if os.name == "nt" else "clear")


def hr(title=""):
    print()
    print(SEP)
    if title:
        print("  " + title)
        print(SEP)
    print()


def ask(prompt, default=""):
    """读一行输入。读到 EOF（被管道喂完）时抛 KeyboardInterrupt 让外层退出。"""
    if default:
        prompt = "%s [%s]: " % (prompt, default)
    else:
        prompt = "%s: " % prompt
    try:
        s = input(prompt).strip()
    except EOFError:
        raise KeyboardInterrupt
    return s if s else default


def run(args, hint=None):
    """执行一条子命令。args[0] 是脚本文件名，会自动补全路径。

    【为什么用 subprocess 而不是 os.system】
      os.system 会把命令交给 Windows 的 cmd.exe 去解析，
      于是路径里的空格、引号、中文都可能被 cmd 自己解释一遍，
      出错时的报错（"文件名、目录名或卷标语法不正确"）还完全看不出是哪儿的问题。
      subprocess + 列表参数是【直接创建进程】，不经过任何 shell ——
      没有二次解析，也就没有这类坑。

    【为什么故意不捕获输出】
      ① 让子脚本的中文直接打到控制台（Windows 下走 Unicode 通道，不会乱码）；
      ② 让你能看见完整的原始输出 —— 那正是要保存的验收证据。
    """
    script = os.path.join(TOOLS_DIR, args[0])
    if not os.path.exists(script):
        print("  !! 找不到脚本: %s" % script)
        print("     确认你在 firmware/tools 目录下运行的是完整工程。")
        return 1

    rest = [str(a) for a in args[1:]]
    cmd = [sys.executable, script] + rest

    print()
    print(LINE)
    print("即将执行：")
    print("  python %s" % " ".join([args[0]] + rest))
    print(LINE)
    print()

    try:
        code = subprocess.run(cmd).returncode
    except OSError as e:
        print("  !! 启动失败: %s" % e)
        return 1
    except KeyboardInterrupt:
        print()
        print("  （已中断）")
        return 130

    print()
    if hint:
        print(LINE)
        print(hint)
        print(LINE)

    print("  [返回码 %d]" % code)
    return code


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def write_text(path, s):
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(s)
    except OSError:
        pass


def load_port():
    return read_text(PORT_FILE)


def save_port(p):
    write_text(PORT_FILE, p)


def find_cli():
    """找 CubeProgrammer 命令行工具。

    顺序：上次记住的路径 → PATH → 环境变量 / 常见安装位置（通配匹配）。
    找到就记住。找不到返回空串（由调用方引导手工指定）。
    """
    cached = read_text(CLI_FILE)
    if cached and os.path.exists(cached):
        return cached

    # PATH 里有就直接用（有些安装包会加进去）
    onpath = shutil.which("STM32_Programmer_CLI")
    if onpath:
        write_text(CLI_FILE, onpath)
        return onpath

    for pat in cli_patterns():
        if os.path.isfile(pat):          # 环境变量给的是完整路径
            write_text(CLI_FILE, pat)
            return pat
        hits = sorted(glob.glob(pat))
        if hits:
            write_text(CLI_FILE, hits[-1])
            return hits[-1]
    return ""


def ensure_log_dir():
    if not os.path.isdir(LOG_DIR):
        try:
            os.makedirs(LOG_DIR)
        except OSError:
            pass
    return LOG_DIR


def need_port():
    """取串口号，没有就问。空字符串 = 取消。"""
    p = load_port()
    p = ask("串口号（如 COM5；填 ? 可列出可用串口）", p)
    if p == "?":
        run(["serial_capture.py", "--list"])
        print()
        p = ask("串口号（如 COM5）")
    if not p:
        print("  已取消。")
        return ""
    save_port(p)
    return p


def preflight():
    """动手前快速自检：pyserial 在不在。"""
    try:
        import serial  # noqa: F401
    except ImportError:
        print()
        print("  !! 这台 Python 里没有 pyserial，串口功能用不了。")
        print("     请先执行：  %s -m pip install pyserial" % sys.executable)
        return False
    return True


# ============================================================
# 各菜单项
# ============================================================
def do_env_check():
    hr("1 · 环境检查")

    print("  Python  : %s" % sys.executable)
    print("  版本    : %s" % sys.version.split()[0])

    try:
        import serial
        print("  pyserial: 已安装 %s   （串口抓包需要它）" % serial.__version__)
    except ImportError:
        print("  pyserial: 【没有装】 → 串口抓包用不了")
        print("            装法：  %s -m pip install pyserial" % sys.executable)

    print()
    print("  脚本齐全性：")
    need = ["multibus.py", "crc16_modbus.py", "golden_vectors.py",
            "make_sample_log.py", "serial_capture.py", "verify_bme280.py",
            "parse_records.py", "stack_budget.py", "run.py"]
    missing = []
    for n in need:
        ok = os.path.exists(os.path.join(TOOLS_DIR, n))
        print("    %-22s %s" % (n, "OK" if ok else "【缺】"))
        if not ok:
            missing.append(n)

    print()
    print("  固件编译产物（两套构建任选其一即可）：")
    for label, p in FLASH_CANDIDATES:
        print("    %-20s %s" % (label, rel_to_fw(p) if os.path.exists(p)
                                else "（还没编译）"))
    print("      提示：CubeIDE 默认只产出 .elf，不产出 .hex —— 这是正常的。")

    print()
    if not missing:
        print("  结论：环境没问题，可以直接进第 2 项。")
    else:
        print("  结论：缺文件，请确认工程是完整的。")

    return 0


def do_demo():
    hr("2 · 用假日志演练（不需要硬件）")
    print("  这一步做两件事：")
    print("    ① 用【真板子实测数据】做基准向量，校验 PC 端算法")
    print("    ② 造一份假日志，把记录解析 + 断电分类跑一遍")
    print()
    print("  ⚠️ 它不能证明固件里的 C 实现是对的 —— 那要靠开机横幅的 #CRC 行来对答案。")

    logs = ensure_log_dir()
    demo = os.path.join(logs, "demo.txt")

    # ★ 第 0 步必须是"独立基准"，不能先跑自己生成的假日志。
    #   踩过的坑：假日志是 make_sample_log.py 用 multibus.py 算出来的，
    #   再用 multibus.py 去复算它 —— 同一条有 bug 的代码自己对自己，
    #   必然报"一致"。结果真上板才发现湿度差了 18 倍。
    #   golden_vectors.py 的期望值来自【固件的 C 实现】，是真正的外部基准。
    rc0 = run(["golden_vectors.py"],
              "★ 这一步才是可信的：期望值来自固件（C），不是来自 Python 自己。\n"
              "  判据：共 30 项、不一致 0 项。\n"
              "  若不一致，先查 multibus.py 里的【运算符优先级】\n"
              "  （Python 的 - 比 >> 高，C 里不是 —— 湿度曾经因此差 18 倍）。")

    rc1 = run(["make_sample_log.py", demo, "--records", "40", "--with-powerloss"])
    rc2 = run(["verify_bme280.py", demo],
              "看最后一行：出现「结论：PASS」。\n"
              "  ⚠️ 但这一步【只证明自洽】—— 假日志和复算用的是同一套代码。\n"
              "     真正的外部校验是上面第 0 步。")
    rc3 = run(["parse_records.py", demo, "--show", "3"],
              "看「校验结果」：应有 40 条有效、1 条半写、1 条未收尾。\n"
              "  那两条异常是【故意造】的，用来验证双 CRC 能不能把两种断电分开 ——\n"
              "  能分开就说明设计是对的（这正是双 CRC 相对单 CRC 的价值）。")

    print()
    if rc0 == 0 and rc2 == 0 and rc3 in (0, 1):
        print("  ✅ 工具链验证通过。接下来可以接板子了（第 3 项起）。")
        if rc3 != 0:
            print()
            print("  ⚠️ 注意第 3 步的「返回码 1」是【正常的】，不是出错：")
            print("     假日志里我故意放了 2 条断电残记录，")
            print("     脚本「发现了异常」正是它该做的事 —— 发现不了才叫有问题。")
            print("     真板子数据正常时，这一步会返回 0。")
    else:
        print("  ⚠️ 上面有步骤没通过，先解决它再上板。")
    return 0


# ---------- 可能的烧录产物 ----------
#   ★ 为什么要列三个：CubeIDE 和 Makefile 把产物放在不同目录，
#     而且 CubeIDE 默认【只产出 .elf，不产出 .hex】。
#     如果写死只烧 build\fw.hex，那么你在 CubeIDE 里编译完再来烧，
#     烧进去的是旧版本 —— 现象是"代码改了却没生效"，极难排查。
FLASH_CANDIDATES = [
    ("CubeIDE Debug 配置",   os.path.join(FW_DIR, "Debug",   "firmware.elf")),
    ("CubeIDE Release 配置", os.path.join(FW_DIR, "Release", "firmware.elf")),
    ("命令行 Makefile 构建", os.path.join(FW_DIR, "build",   "fw.hex")),
]


def rel_to_fw(p):
    """把绝对路径显示成相对 firmware 的短路径。

    ★ 为什么要 try：os.path.relpath 在【跨盘符】时会直接抛 ValueError
      （例如 Windows 上 C: 的路径相对 D: 求相对路径）。
      真实使用中产物都在工程内部，不会触发；但显示路径这种辅助功能
      不该因为一个格式问题把整个程序打挂，所以退化成显示绝对路径。
    """
    try:
        return os.path.relpath(p, FW_DIR)
    except ValueError:
        return p


def newest_source_mtime():
    """Core 下最新一个源文件的修改时间，用来判断"固件是不是比源码旧"。"""
    newest = 0.0
    for root, _dirs, files in os.walk(os.path.join(FW_DIR, "Core")):
        for fn in files:
            if fn.endswith((".c", ".h", ".s")):
                try:
                    m = os.path.getmtime(os.path.join(root, fn))
                except OSError:
                    continue
                if m > newest:
                    newest = m
    return newest


def pick_firmware():
    """挑一个要烧的固件，返回路径；返回空串 = 取消。"""
    found = []
    for label, p in FLASH_CANDIDATES:
        if os.path.exists(p):
            found.append((label, p, os.path.getmtime(p)))
    if not found:
        return ""

    found.sort(key=lambda x: x[2], reverse=True)
    src_mtime = newest_source_mtime()

    print("  找到以下编译产物（按新旧排序）：")
    print()
    print("    #  来源                  修改时间          文件")
    print("    -  ------------------  ----------------  ------------------------")
    for i, (label, p, m) in enumerate(found, 1):
        t = time.strftime("%Y-%m-%d %H:%M", time.localtime(m))
        print("    %d  %-18s  %s  %s" % (i, label, t, rel_to_fw(p)))
    print()

    if src_mtime > found[0][2]:
        t = time.strftime("%Y-%m-%d %H:%M", time.localtime(src_mtime))
        print("  ⚠️ 你的源码在 %s 有改动，比【最新】的固件还新。" % t)
        print("     说明编译产物是旧的。烧旧固件的现象是「代码明明改了却没生效」，")
        print("     非常难查 —— 建议先重新编译：")
        print("       · CubeIDE 里：Project > Build Project（Ctrl+B）")
        print("       · 命令行   ：在 firmware 目录执行 make")
        print()
        ans = ask("仍然要烧这个旧固件吗？（y = 继续 / 回车 = 退出去重新编译）")
        if ans.lower() not in ("y", "yes"):
            print("  已取消 —— 去重新编译吧。")
            return ""

    if len(found) > 1:
        sel = ask("烧哪一个？（回车 = 1，即最新的那个）", "1")
        try:
            idx = int(sel) - 1
            if not (0 <= idx < len(found)):
                idx = 0
        except ValueError:
            idx = 0
    else:
        idx = 0

    label, path, _m = found[idx]
    print()
    print("  已选：%s" % label)
    print("  固件：%s" % path)
    print("  大小：%d 字节" % os.path.getsize(path))
    return path


def do_flash():
    hr("8 · 烧录固件（用 ST-Link 把固件写进芯片）")
    print("  ★ 这一步【完全不需要 CubeIDE】，但 CubeIDE 编译出来的固件同样能烧。")
    print()
    print("  动手前确认两件事：")
    print("    ① 板子插好，ST-Link 的 SWD 四根线接好（SWDIO/SWCLK/GND/3.3V）")
    print("    ② CubeIDE 的调试会话已关闭（否则它占着 ST-Link，会报 No ST-LINK detected）")
    print()

    fw_file = pick_firmware()
    if not fw_file:
        if not any(os.path.exists(p) for _l, p in FLASH_CANDIDATES):
            print("  !! 一个编译产物都没找到。先编译：")
            print("       · CubeIDE 里：Project > Build Project（Ctrl+B）")
            print("       · 命令行   ：在 firmware 目录执行 make")
        return 1
    print()

    cli = find_cli()
    if not cli:
        print("  没自动找到 STM32_Programmer_CLI.exe。")
        print("  它通常藏在 CubeIDE 的插件目录里（目录名里带 cubeprogrammer），")
        print("  形如： <CubeIDE 安装目录>\\STM32CubeIDE\\plugins\\"
              "com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*\\tools\\bin\\")
        print("  也可以在文件管理器里搜文件名 STM32_Programmer_CLI.exe。")
        print("  提示：设置环境变量 %s 为完整路径，下次就能自动找到。" % CLI_ENV_VAR)
        print()
        p = ask("把它的完整路径粘进来（直接回车 = 取消）")
        if not p or not os.path.exists(p):
            print("  已取消。")
            return 1
        write_text(CLI_FILE, p)
        cli = p
        print("  已记住这个路径，下次不用再输。")

    print("  烧录器: %s" % cli)
    print()
    print(LINE)
    print("即将执行（等价于在命令行敲这一句）：")
    print('  "%s" -c port=SWD freq=4000 -d "%s" -v -rst' % (cli, fw_file))
    print(LINE)
    print()
    print("  每个参数的意思：")
    print("    -c port=SWD freq=4000  用 SWD 口连，4 MHz。")
    print("                           克隆版 ST-Link 用这个速度最稳，调快了容易连不上。")
    print("    -d <file>              把固件写进芯片。")
    print("                           .hex 和 .elf 都自带地址信息，")
    print("                           所以不像 .bin 那样要另写 0x08000000。")
    print("    -v                     写完回读比对。")
    print("                           ★ 一定要加：不加就只是'写进去了'，不保证写对。")
    print("    -rst                   软复位，让新程序立刻开始跑。")
    print("                           用软复位而不是硬件复位，是因为它不依赖 NRST 那根线。")
    print()

    ans = ask("确认烧录？（y = 开始 / 直接回车 = 取消）")
    if ans.lower() not in ("y", "yes"):
        print("  已取消。")
        return 1

    print()
    try:
        code = subprocess.run(
            [cli, "-c", "port=SWD", "freq=4000", "-d", fw_file, "-v", "-rst"]
        ).returncode
    except OSError as e:
        print("  !! 启动 CubeProgrammer 失败: %s" % e)
        return 1
    except KeyboardInterrupt:
        print()
        print("  （已中断）")
        return 130

    print()
    print(LINE)
    if code == 0:
        print("  ✅ 烧录完成。请在上面确认这两行出现过：")
        print("       Download verified successfully")
        print("       Software reset is performed")
        print()
        print("  ⚠️ 万一后面串口一个字都没有 —— 【先拔插一次 USB】再试。")
        print("     调试器复位后内核可能停在暂停态，程序根本没被执行，")
        print("     这不是代码问题。断电重上电是最可靠的验证方式。")
        print()
        print("  下一步：回菜单选 9，抓 15 秒串口看开机横幅。")
        print("  要核对的四行（详见 docs\\08_怎么打开工程与烧录.md 第 4.2 节）：")
        print("    #CLK,got:    四个数字要和下一行 expect 完全一致")
        print("    #UART,BRR=   625，且 DWT=ok")
        print("    #CRC         PASS")
        print("    之后每秒一行   ← 说明 FreeRTOS 三任务都在跑")
    else:
        print("  ✗ 烧录失败（返回码 %d）。常见原因：" % code)
        print("     · No ST-LINK detected      → CubeIDE 开着调试会话，先关掉")
        print("     · 连不上 / target 无响应    → 检查 SWD 四根线、板子有没有供电")
        print("     · 芯片读保护 (RDP)          → 需要先全片擦除")
        print()
        print("  想知道板子到底连上没有，可以先只读地探一下（不改任何东西）：")
        print('     "%s" -c port=SWD' % cli)
    print(LINE)
    return code


def do_monitor():
    hr("9 · 看串口输出（只监视，不改板子）")
    print("  就是'打开串口看一眼'，不做任何校验、不碰芯片。")
    print("  用途：烧完固件确认板子在跑；或板子跑着的时候随手看两眼。")
    print()
    print("  ★ 先关掉别的串口助手 —— 串口一次只能被一个程序打开。")

    if not preflight():
        return 1
    port = need_port()
    if not port:
        return 1

    logs = ensure_log_dir()
    out = os.path.join(logs, "monitor.txt")

    secs = ask("看多少秒", "15")
    try:
        secs = str(int(float(secs)))
    except ValueError:
        secs = "15"

    rc = run(["serial_capture.py", "--port", port, "--out", out,
              "--seconds", secs],
             "输出【同时】存到了 logs\\monitor.txt（下次跑会覆盖，要留档先改名）。\n"
             "  开机横幅要核对这四行：\n"
             "    #CLK,got:    四个数字 = 下一行 expect\n"
             "    #UART,BRR=   625，DWT=ok\n"
             "    #CRC         PASS（这行过了，PC 端校验才成立）\n"
             "    之后每秒一行   ← FreeRTOS 三任务在跑\n"
             "  看不到任何输出时的排查顺序：\n"
             "    ① 拔插一次 USB（最常见原因）\n"
             "    ② TX/RX 是不是接反了（要交叉：板子 PA9 → CH340 RX）\n"
             "    ③ 波特率是不是 115200")
    return rc


def do_bme280():
    hr("3 · 验证「温湿压 PC 端复算逐位一致」")
    print("  对应简历项目经历第 3 条的最后半句。")
    print()
    print("  它会做两件事：")
    print("    ① 抓日志：先发 c 取 33 个校准系数，再发 5 次 m 触发 5 次测量")
    print("    ② PC 端【只用原始 ADC 值 + 校准系数】独立算一遍，和固件输出逐位比对")
    print()
    print("  前提：板子插好、已烧固件、串口助手【先关掉】（串口一次只能一个程序占用）")

    if not preflight():
        return 1
    port = need_port()
    if not port:
        return 1

    logs = ensure_log_dir()
    out = os.path.join(logs, "bme.txt")

    run(["serial_capture.py", "--port", port, "--out", out,
         "--seq", "c,m,m,m,m,m", "--gap", "2"])

    rc = run(["verify_bme280.py", out],
             "判据：最后一行出现「结论：PASS」，并且上面「不一致 0 项」。\n"
             "  记下「共 N 项」这个数字 —— 面试/论文里可以引用。\n"
             "  日志存到了 logs\\bme.txt，别删，这是证据。")
    return rc


def do_records():
    hr("4 · 验证「32 B 记录 PC 逐字节双 CRC 校验」")
    print("  对应简历项目经历第 4 条。")
    print()
    print("  它会：① 把 Flash 里已存的记录全倒出来  ② PC 端逐条重算头、尾两个 CRC")
    print()
    print("  ⏱ 耗时提示：115200 下大约每秒 1.5 KB，")
    print("     1000 条记录约 50 秒，10000 条约 8 分钟。先跑少一点够用。")

    if not preflight():
        return 1
    port = need_port()
    if not port:
        return 1

    logs = ensure_log_dir()
    out = os.path.join(logs, "records.txt")
    csv = os.path.join(logs, "records.csv")

    run(["serial_capture.py", "--port", port, "--out", out,
         "--seq", "r", "--stop-marker", "#DUMP,end"])

    rc = run(["parse_records.py", out, "--csv", csv, "--show", "5"],
             "判据：最后出现「结论: PASS」。\n"
             "  「未收尾」不是 0 也不一定是错 —— 那可能是断电测试留下的痕迹，\n"
             "  说明双 CRC 正在工作。\n"
             "  logs\\records.csv 可以直接用 Excel 打开画图，放进论文。")
    return rc


def do_stack():
    hr("5 · 算栈水位压缩比（填简历那个 [实测后填]）")
    print("  对应简历项目经历第 1 条的括号里那个数字。")
    print()
    print("  ⚠️ 这个数字【必须是实测的】，不许用示例值。")
    print("     所以要先让板子跑一段（越久越准），期间它会自动打 #STK 行。")
    print()
    print("  建议：先让板子连续跑 1~24 小时；也可以边跑边发 s 命令立即打印。")

    if not preflight():
        return 1
    port = need_port()
    if not port:
        return 1

    logs = ensure_log_dir()
    out = os.path.join(logs, "stack.txt")

    secs = ask("抓取时长（秒），例如 300", "300")
    try:
        secs = str(int(secs))
    except ValueError:
        secs = "300"

    alloc = ask("当时固件的栈分配 acq/proc/comm（字），宽裕档是 256/256/384",
                "acq=256,proc=256,comm=384")
    base = ask("对照基准（每任务统一多少字）", "512")

    run(["serial_capture.py", "--port", port, "--out", out,
         "--seconds", secs])

    rc = run(["stack_budget.py", out, "--alloc", alloc, "--baseline", base],
             "看「可以直接填进简历红色占位符的结论」那一节。\n"
             "  ★ 三条硬规矩：\n"
             "    ① --alloc 必须与当时烧录的固件一致（开机横幅 #BUILD 行可佐证）\n"
             "    ② 压缩比必须说明是相对哪个基准算的，否则面试官会追问到底\n"
             "    ③ 填完之后按推荐值重新编译再跑一次，确认剩余水位仍 > 0")
    return rc


def do_selftest():
    hr("6 · 跑 W4 全量自检")
    print("  一次跑完这四块，每块都有可留存的输出：")
    print("    ① 四种 SPI 模式回环（需要一根杜邦线短接 PA7 与 PA6）")
    print("    ② 四种模式分别读 W25Q64 的 JEDEC ID  ← 最硬的证据")
    print("    ③ 32 B 记录闭环（写 256 条 + 三重比对，跨页跨扇区）")
    print("    ④ 断电三情况（验证头尾双 CRC 能区分开）")
    print()
    print("  ⚠️ ④ 会擦掉芯片最后 8 KB 的【自检暂存区】—— 不会碰你的真实记录。")

    if not preflight():
        return 1
    port = need_port()
    if not port:
        return 1

    logs = ensure_log_dir()
    out = os.path.join(logs, "selftest.txt")

    rc = run(["serial_capture.py", "--port", port, "--out", out,
              "--seq", "t", "--stop-marker", "W4 自检结束"],
             "要核对的几行（详细判据见 docs\\05_验收手册.md 第 6 节）：\n"
             "  #SPI4  四种模式回环应【全部 MATCH】\n"
             "         （全 MATCH 才是预期！回环看不到模式配对错误，原因见 docs\\04）\n"
             "  #SPI4,MISMATCH  ...shifted=YES\n"
             "  #SPIID 读通模式数=2（Mode0 与 Mode3 得 EF 40 17）\n"
             "  #REC   written=256,mismatch=0,result=PASS\n"
             "  #PWR   case1/case2/case3 全 PASS\n"
             "完整日志存到了 logs\\selftest.txt")
    return rc


def do_crc():
    hr("7 · CRC 标准检查值（和板子对答案）")
    print("  这一步是为了确认：PC 端和固件是不是【同一套 CRC 算法】。")
    print("  两边不一致的话，后面所有校验都会失败，而且看起来像 Flash 坏了。")
    print()
    print("  固件那边看开机横幅的这一行：")
    print("      #CRC,check='123456789',got=0x4B37,expect=0x4B37,PASS")
    print()

    rc = run(["crc16_modbus.py", "--selftest"],
             "判据：PC 端这里必须也是 0x4B37。\n"
             "  两边一样 → 算法一致，可以放心做后面的校验。")
    return rc


# ============================================================
# 菜单
# ============================================================
def print_menu():
    clear()
    print(SEP)
    print("  多总线数据采集终端 · PC 端校验工具")
    print("  ——你不需要懂 Python，选数字回车就行——")
    print(SEP)

    print("  本机 Python : %s" % sys.version.split()[0])
    try:
        import serial
        print("  pyserial    : 已装 %s" % serial.__version__)
    except ImportError:
        print("  pyserial    : 【没装】→ 串口功能不可用，先跑第 1 项看提示")

    p = load_port()
    print("  上次串口号  : %s" % (p if p else "（还没设过）"))
    print("  日志目录    : %s" % LOG_DIR)
    print()
    print("  【不需要硬件，先做这两步】")
    print("    1  检查环境")
    print("    2  用假日志演练一遍          ← 建议先做，确认工具是通的")
    print()
    print("  【第一次上板：先把固件烧进去】")
    print("    8  烧录固件（自动挑最新的产物）  ← 板子到手第一件事，不用 CubeIDE")
    print("    9  看串口输出（只监视，不改板子）")
    print()
    print("  【板子已烧好固件后，做这些验证】")
    print("    3  温湿压 PC 端复算逐位一致    → 简历第 3 条")
    print("    4  32 B 记录 PC 逐字节双 CRC   → 简历第 4 条")
    print("    5  算栈水位压缩比              → 简历第 1 条那个 [实测后填]")
    print("    6  W4 全量自检（四模式/记录/断电）")
    print("    7  对 CRC 标准检查值（和板子对答案）")
    print()
    print("    p  改串口号（平时不用：输串口号那一步直接填新的就会记住）")
    print("    0  退出")
    print()
    print("  不知道从哪开始？ → 先读 docs\\08_怎么打开工程与烧录.md")
    print()


def main():
    while True:
        print_menu()
        try:
            c = ask("请选择").strip()
        except KeyboardInterrupt:
            print()
            return 0

        try:
            if c == "1":
                do_env_check()
            elif c == "2":
                do_demo()
            elif c == "3":
                do_bme280()
            elif c == "4":
                do_records()
            elif c == "5":
                do_stack()
            elif c == "6":
                do_selftest()
            elif c == "7":
                do_crc()
            elif c == "8":
                do_flash()
            elif c == "9":
                do_monitor()
            elif c in ("p", "P"):
                save_port(ask("新的串口号（如 COM5）"))
                print("  已记住。")
            elif c == "0":
                print()
                print("  再见。日志都在 logs\\ 目录里。")
                return 0
            else:
                print("  没有这个选项，请输入 1~9、p 或 0。")
        except KeyboardInterrupt:
            print()
            print("  （已中断）")
            return 0

        print()
        try:
            ask("按回车回菜单")
        except KeyboardInterrupt:
            return 0


if __name__ == "__main__":
    sys.exit(main())
