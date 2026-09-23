# ============================================================
# Makefile —— 多总线数据采集终端（STM32F103C8T6）
#
# 用法：
#     make            编译，产出 build/fw.elf + fw.hex + fw.bin
#     make size       看 Flash / RAM 占用
#     make clean      清理
#     make flash      用 STM32CubeProgrammer 烧录（需要 PATH 里有 CLI 或改 FLASHER）
#
# 【工具链】
#   默认用 PATH 里的 arm-none-eabi-*。若没有，用 CubeIDE 自带的那份即可：
#   在 CubeIDE 安装目录的 plugins/ 下找
#     com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin
#   然后 make TOOLCHAIN="<那个 bin 目录>"
#
# 【为什么不用 CubeIDE 生成的 Debug/makefile】
#   那份是 IDE 自动生成的（文件头写着 Do not edit），
#   它和 .cproject 绑定，手改会被下次生成覆盖。
#   本文件是独立的、可读的、只依赖工具链本身。
# ============================================================

# ---------- 工具链 ----------
# 默认使用 PATH 里的 arm-none-eabi-*，不写死任何本机绝对路径。
# 若工具链不在 PATH（例如直接用 CubeIDE 自带的），任选一种方式指定：
#   ① 命令行覆盖：  make TOOLCHAIN="D:/STM32CubeIDE_x.y/.../tools/bin"
#   ② 导入工程前导出：export PATH="$PATH:/path/to/tools/bin"
#   ③ 改下面这行的默认值
#
#   CubeIDE 自带工具链的位置形如：
#     <CubeIDE 安装目录>/STM32CubeIDE/plugins/
#       com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin
TOOLCHAIN ?=
ifneq ($(strip $(TOOLCHAIN)),)
  PREFIX := $(TOOLCHAIN)/arm-none-eabi-
else
  PREFIX := arm-none-eabi-
endif
CC        := $(PREFIX)gcc
AS        := $(PREFIX)gcc -x assembler-with-cpp
CP        := $(PREFIX)objcopy
SZ        := $(PREFIX)size
NM        := $(PREFIX)nm

# STM32_Programmer_CLI 通常随 CubeIDE 一起安装（CubeProgrammer 插件自带），
# 也可能单独装。默认按 PATH 查找；不在 PATH 就用 FLASHER=... 覆盖，
# 或用环境变量 STM32_CUBEPROGRAMMER 指向完整路径。
FLASHER   ?= STM32_Programmer_CLI

# ---------- 建 / 删目录的命令 ----------
# ★ 为什么做成变量：Windows 上 make 用哪个 shell 决定了语法。
#   CubeIDE 自带的 make 在 Windows 上默认用 cmd.exe（实测：mkdir -p 会报错，
#   cmd 的 mkdir 没有 -p 选项）。而在 Git Bash / MSYS 里跑 make 用的是 sh。
#   默认值按 cmd.exe 写，够覆盖 99% 场景。
#   【如果你在 Git Bash 里跑 make】，加两个覆盖：
#       make MKDIR_CMD="mkdir -p" CLEAN_CMD="rm -rf"
MKDIR_CMD ?= mkdir
CLEAN_CMD ?= rmdir /S /Q

# ---------- 目标 ----------
TARGET    := fw
BUILD_DIR := build

# ---------- 源文件 ----------
C_SOURCES := \
  Core/Src/main.c \
  Core/Src/clock_init.c \
  Core/Src/delay_us.c \
  Core/Src/system_stm32f1xx.c \
  Core/Src/stm32f1xx_it.c \
  Core/Src/uart_dbg.c \
  Core/Src/log.c \
  Core/Src/i2c_soft.c \
  Core/Src/bme280.c \
  Core/Src/spi_soft.c \
  Core/Src/w25q64.c \
  Core/Src/crc16.c \
  Core/Src/recorder.c \
  Core/Src/buzzer.c \
  Core/Src/app_tasks.c \
  Core/Src/self_test.c \
  Middlewares/Third_Party/FreeRTOS/Source/list.c \
  Middlewares/Third_Party/FreeRTOS/Source/queue.c \
  Middlewares/Third_Party/FreeRTOS/Source/tasks.c \
  Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c \
  Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c

ASM_SOURCES := Core/Startup/startup_stm32f103xb.s

# ---------- 头文件路径 ----------
C_INCLUDES := \
  -ICore/Inc \
  -IDrivers/CMSIS/Include \
  -IDrivers/CMSIS/Device/ST/STM32F1xx/Include \
  -IMiddlewares/Third_Party/FreeRTOS/Source/include \
  -IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3

# ---------- 宏定义 ----------
# STM32F103xB = 中容量 F103（64 KB Flash / 20 KB RAM），
# 它决定 stm32f103xb.h 里 #include 哪一份具体型号头文件
C_DEFS := -DSTM32F103xB

# ---------- 编译选项 ----------
CPU      := -mcpu=cortex-m3 -mthumb -mfloat-abi=soft
# -Og        : 为调试优化的档位（比 -O0 代码好读，比 -O2 好单步）
# -Wall -Wextra : 打开全部常用告警。**本项目要求零告警**
# -ffunction-sections -fdata-sections + 链接期 --gc-sections :
#               把没被引用的函数整段丢掉，能省几千字节 Flash
# -finput-charset / -fexec-charset :
#               源码和输出都用 UTF-8，保证串口打出的中文是 UTF-8 字节
#
# ★ 注意：这里【不能】写 -MF"$(@:%.o=%.d)"
#   CFLAGS 是 := 立即展开的，解析这一刻 $@ 还不存在，会展开成空的 -MF""，
#   于是 GCC 把紧跟其后的那个参数当成依赖文件名 —— 报错信息会很误导
#   （fatal error: opening dependency file -Wa,...）。只写 -MMD -MP 就够了，
#   GCC 会自动按 -o 的名字生成同名 .d 文件。
CFLAGS   := $(CPU) $(C_DEFS) $(C_INCLUDES) \
            -Og -g3 -Wall -Wextra \
            -ffunction-sections -fdata-sections \
            -finput-charset=UTF-8 -fexec-charset=UTF-8 \
            -MMD -MP

LDFLAGS  := $(CPU) -TSTM32F103C8TX_FLASH.ld \
            -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref \
            -Wl,--gc-sections \
            -specs=nano.specs -specs=nosys.specs

LIBS     := -lc -lm

# ---------- 派生 ----------
OBJECTS  := $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

# ============================================================
# 目标
# ============================================================
.PHONY: all clean size flash symbols check-src

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O binary -S $< $@

# ★ 不能用 mkdir -p：
#   CubeIDE 自带的 make 在 Windows 上默认拿 cmd.exe 当 shell，
#   cmd 的 mkdir 没有 -p 选项，会把它当成一个要创建的目录名，
#   报出"子目录或文件 -p 已经存在"这种莫名其妙的错。
#   换成不带参数的 mkdir —— cmd 和 sh 都认；开头的 - 让"已存在"不算错误。
$(BUILD_DIR):
	-$(MKDIR_CMD) $@

# ★ 注意这里用 $< （第一个依赖 = 那个 .elf），不能用 $@
#   $@ 在配方里指的是"目标名"，也就是 size 这个词本身，
#   写成 $@ 会变成 size.exe -A size —— 找不到文件。
size: $(BUILD_DIR)/$(TARGET).elf
	@echo "==== 段明细 ===="
	$(SZ) -A $<
	@echo ""
	@echo "==== 汇总（text=Flash，data+bss=RAM）===="
	$(SZ) $<
	@echo ""
	@echo "Flash 预算 65536 字节，RAM 预算 20480 字节"

# 把符号表整个导出到文件，自己搜 SysTick_Handler 等（不用 grep —— 它在 cmd.exe 里不存在）
symbols: $(BUILD_DIR)/$(TARGET).elf
	$(NM) $< > $(BUILD_DIR)/symbols.txt
	@echo "符号表已导出到 $(BUILD_DIR)/symbols.txt"
	@echo "要确认的中断处理函数（每个必须【只有一处定义】）："
	@echo "  SysTick_Handler / PendSV_Handler / SVC_Handler"
	@echo "  USART1_IRQHandler / HardFault_Handler"
	@echo "若同一个名字出现两次，链接期就会报重复定义 —— 那说明两个 RTOS 层都被编进来了。"

flash: $(BUILD_DIR)/$(TARGET).hex
	$(FLASHER) -c port=SWD -w $< -v -rst

# ============================================================
# 一致性自检：磁盘上的源文件 与 C_SOURCES 清单 是否对得上
#
# 【为什么需要这个】
#   本工程现在有【两套】构建方式：
#     · CubeIDE 托管构建 —— 自动扫描 Core/Middlewares/Drivers 下所有 .c
#     · 本 Makefile       —— 手写 C_SOURCES 清单（见上）
#   后果：你新增一个 .c 时，CubeIDE 里能编过，make 却会报"找不到符号"。
#   这个目标就是用来提前发现这种分叉的。两节都空 = 一致。
# ============================================================
DISK_C   := $(sort $(notdir $(wildcard Core/Src/*.c)))
# 只取 Core/Src 那一批来比 —— C_SOURCES 里还有 FreeRTOS 的 list.c/queue.c…，
# 它们同名但不在 Core/Src 下，不做过滤会把它们误报成"路径写错了"。
LISTED_C := $(sort $(notdir $(filter Core/Src/%,$(C_SOURCES))))

check-src:
	@echo "==== [1] 在磁盘上、但没进 C_SOURCES 的（要让 make 也编，得手工加进 C_SOURCES）===="
	@echo "  $(filter-out $(LISTED_C),$(DISK_C))"
	@echo ""
	@echo "==== [2] 在 C_SOURCES 里、但磁盘上没有的（路径写错了）===="
	@echo "  $(filter-out $(DISK_C),$(LISTED_C))"
	@echo ""
	@echo "==== 说明 ===="
	@echo "  CubeIDE 自动扫描源文件夹，新增 .c 不用改任何配置；"
	@echo "  本 Makefile 用 C_SOURCES 手写清单，新增 .c 必须手工加。"
	@echo "  上面两节都为空 = 一致。"

# 删构建目录。
# ★ cmd.exe 和 sh 的删除命令不一样，所以这里用变量，默认按 cmd.exe 写。
#   在 Git Bash / MSYS 里跑 make 时，加一句覆盖即可：
#       make clean CLEAN_CMD="rm -rf"
clean:
	@echo "删除 $(BUILD_DIR)  ……  (命令：$(CLEAN_CMD) $(BUILD_DIR))"
	-$(CLEAN_CMD) $(BUILD_DIR)
	@echo "如果你在 Git Bash 里跑，请用：make clean CLEAN_CMD=\"rm -rf\""

-include $(wildcard $(BUILD_DIR)/*.d)
