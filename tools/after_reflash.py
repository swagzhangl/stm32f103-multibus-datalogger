# -*- coding: utf-8 -*-
"""
重刷固件后的一口气流程：
  清空记录区 → 采 3 分钟 → 倒出记录 → 存日志

用法：python run_after_reflash.py [采集秒数，默认 180]
前置：串口助手必须关掉（COM8 一次只能被一个程序占用）
"""
import sys, os, time

# 串口与日志目录都不依赖本机固定路径：
#   端口 可用环境变量覆盖（set MULTIBUS_PORT=COM5），默认 COM8
#   日志目录由本脚本位置推导（tools/ 的上一级 + logs/）
PORT = os.environ.get("MULTIBUS_PORT", "COM8")
BAUD = 115200
_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(os.path.dirname(_TOOLS_DIR), "logs")
OUTFILE = os.path.join(LOG_DIR, "records_v2.txt")

SETTLE = float(sys.argv[1]) if len(sys.argv) > 1 else 180.0

out = []
def P(s=""):
    out.append(s)

try:
    import serial
except ImportError:
    P("!! 没有 pyserial"); sys.stdout.write("\n".join(out)); sys.exit(1)

os.makedirs(LOG_DIR, exist_ok=True)

try:
    s = serial.Serial(PORT, BAUD, timeout=1, dsrdtr=False, rtscts=False)
except Exception as e:
    P("!! 打不开 %s :: %s" % (PORT, e))
    P("   先关掉串口助手再跑。")
    sys.stdout.write("\n".join(out)); sys.exit(2)

buf = []
def pump(sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        n = s.in_waiting
        if n:
            buf.append(s.read(n).decode("utf-8", "replace"))
        time.sleep(0.04)

def snap():
    return "".join(buf)

def send(c):
    s.write((c + "\r\n").encode("ascii"))

# ---------- 0) 先听 3 秒，确认板子在跑 ----------
P("[0] 监听 3 秒，确认板子在跑……")
pump(3)
if "] seq=" not in snap():
    P("!! 3 秒内没收到采集行。")
    P("   检查：TX/RX 是否交叉、波特率 115200、板子是否在运行。")
    P("   抓到的内容：")
    P("   " + repr(snap()[:300]))
    s.close()
    sys.stdout.write("\n".join(out)); sys.exit(3)
P("    OK，板子在跑")

# ---------- 1) 清空记录区（发两次 e，中间隔 3 秒） ----------
P("")
P("[1] 清空记录区 —— 第一次 e（应回 #ERASE,ARMED）")
mark = len(snap())
send("e")
pump(2.5)
tail = snap()[mark:]
P("    回: " + (tail.strip().splitlines()[-1] if tail.strip() else "（无响应）"))

P("[1] 第二次 e（间隔 3 秒后，应回 #ERASE,OK）")
mark = len(snap())
send("e")
pump(4)
tail = snap()[mark:]
P("    回: " + (tail.strip().splitlines()[0] if tail.strip() else "（无响应）"))
if "ERASE,OK" not in tail:
    P("    ⚠️ 没看到 #ERASE,OK —— 下面先继续，但记录数可能不是从 0 开始")

# ---------- 2) 看状态 ----------
P("")
P("[2] 发 p 查状态")
mark = len(snap())
send("p")
pump(2)
for line in snap()[mark:].splitlines():
    if line.startswith("#STA"):
        P("    " + line)

# ---------- 3) 采集 N 秒 ----------
P("")
P("[3] 采集 %d 秒（约 %d 条记录）……" % (int(SETTLE), int(SETTLE)))
n0 = snap().count("] seq=")
t0 = time.time()
while time.time() - t0 < SETTLE:
    pump(5)
    n = snap().count("] seq=")
    el = int(time.time() - t0)
    if el % 30 < 6:
        P("    %3d s  已见采集行 %d" % (el, n))
pump(1)
P("    采集结束，共见采集行 %d 条" % (snap().count("] seq=") - n0))

# ---------- 4) 倒出记录 ----------
P("")
P("[4] 发 r 倒出记录区……")
mark = len(snap())
send("r")
t0 = time.time()
while time.time() - t0 < 90:
    pump(2)
    if "#DUMP,end" in snap()[mark:]:
        break
tail = snap()[mark:]
dump_lines = [l for l in tail.splitlines() if l.startswith("REC,")]
P("    #DUMP,begin 行: " + ([l for l in tail.splitlines() if "#DUMP,begin" in l] or ["（未收到）"])[0])
P("    收到 REC 行 %d 条" % len(dump_lines))
P("    #DUMP,end   行: " + ([l for l in tail.splitlines() if "#DUMP,end" in l] or ["（未收到）"])[0])

# ---------- 5) 栈水位 ----------
P("")
P("[5] 发 s 取栈水位（为后面算压缩比）")
mark = len(snap())
send("s")
pump(2)
for line in snap()[mark:].splitlines():
    if line.startswith("#STK"):
        P("    " + line)

s.close()

# ---------- 6) 存日志 ----------
try:
    with open(OUTFILE, "w", encoding="utf-8") as f:
        f.write("# captured by run_after_reflash.py at %s\n" %
                time.strftime("%Y-%m-%dT%H:%M:%S"))
        f.write("# port=%s baud=%d\n" % (PORT, BAUD))
        f.write(snap())
    P("")
    P("[6] 日志已存: %s (%d 字节)" % (OUTFILE, os.path.getsize(OUTFILE)))
except OSError as e:
    P("!! 写日志失败: %s" % e)

sys.stdout.write("\n".join(out))
