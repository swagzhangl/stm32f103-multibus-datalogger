# -*- coding: utf-8 -*-
"""
长跑开始：清空记录区，确认从 0 开始，并记下起始时间。

用法： python longrun_start.py
"""
import sys, os, time, json
import serial

# 串口与日志目录都不依赖本机固定路径（端口可用 MULTIBUS_PORT 环境变量覆盖）
PORT = os.environ.get("MULTIBUS_PORT", "COM8")
BAUD = 115200
_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(os.path.dirname(_TOOLS_DIR), "logs")
META = os.path.join(LOG_DIR, "longrun_meta.json")

out = []
def P(s=""):
    out.append(s)

os.makedirs(LOG_DIR, exist_ok=True)

try:
    s = serial.Serial(PORT, BAUD, timeout=1, dsrdtr=False, rtscts=False)
except Exception as e:
    P("!! 打不开 %s :: %s" % (PORT, e))
    P("   先关掉串口助手。")
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

pump(2)
if "] seq=" not in snap():
    P("!! 没收到采集行，板子可能没在跑"); s.close()
    sys.stdout.write("\n".join(out)); sys.exit(3)

# ---------- 1) 清空记录区 ----------
# ★ 新固件的 e 会擦【整个记录区】1024 个扇区，约 30 s ~ 7 min。
#   擦除期间 comm 任务占着 SPI 锁，【p / s 都不会有应答】—— 这是正常的。
P("")
P("[1] 第一次 e（应回 #ERASE,ARMED）")
send("e"); pump(2.5)
tail = snap()
arm = [l for l in tail.splitlines() if l.startswith("#ERASE")]
P("    回: " + (arm[-1] if arm else "（没收到 #ERASE）"))

P("[2] 等 3 秒后第二次 e（触发全片擦除，需 30 秒~几分钟）")
mark = len(snap())
send("e"); pump(3)

t0 = time.time()
ok_line = None
while time.time() - t0 < 600:
    pump(5)
    hits = [l for l in snap()[mark:].splitlines() if l.startswith("#ERASE")]
    if hits and "OK" in hits[-1]:
        ok_line = hits[-1]
        break
    el = int(time.time() - t0)
    if el % 30 < 6:
        P("    %3d s  擦除中……（期间串口安静、p/s 无应答都是正常的）" % el)

P("    回: " + (ok_line if ok_line else "⚠️ 10 分钟没等到 #ERASE,OK，稍后用 p 确认 records"))

# ---------- 2) 看状态 / 栈水位（擦除结束后才可能有应答） ----------
P("")
P("[3] 发 p 查状态（应显示 records 很小或 0）")
t0 = time.time(); sta = []
while time.time() - t0 < 20 and not sta:
    m2 = len(snap())
    send("p"); pump(3)
    sta = [l for l in snap()[m2:].splitlines() if l.startswith("#STA")]
P("    " + (sta[-1] if sta else "（还是没应答，稍后再看）"))

P("[4] 发 s 取栈水位基线")
t0 = time.time(); stk = []
while time.time() - t0 < 20 and not stk:
    m3 = len(snap())
    send("s"); pump(3)
    stk = [l for l in snap()[m3:].splitlines() if l.startswith("#STK")]
P("    " + (stk[-1] if stk else "（还是没应答，稍后再看）"))
s.close()

t0 = time.strftime("%Y-%m-%d %H:%M:%S")
try:
    json.dump({"start": t0, "port": PORT},
              open(META, "w", encoding="utf-8"), ensure_ascii=False)
except OSError:
    pass

P("")
P("=" * 66)
P("★ 长跑开始时间 = %s" % t0)
P("=" * 66)
P("接下来：")
P("  1. 板子别断电、别拔 USB（PC 别关机；有条件就接独立电源）")
P("  2. 中途想采样： python tools\\longrun_sample.py   （只发 s/p，不占 SPI 锁）")
P("  3. 千万别发 t 或 r —— 它们长时间占 SPI 锁，会造成成片 store=skip")
P("  4. 到点收尾：   python tools\\longrun_finish.py")
sys.stdout.write("\n".join(out))
