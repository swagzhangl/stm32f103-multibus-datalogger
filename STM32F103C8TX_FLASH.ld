/* ============================================================
 * STM32F103C8TX_FLASH.ld —— 链接脚本（64 KB Flash / 20 KB RAM）
 *
 * 【这个文件为什么和 FreeRTOS 移植有关】
 *   不用 CubeMX 自己移植 FreeRTOS 要接三件事，这是第 3 件：
 *   链接脚本里的【栈与堆】。
 *
 *   本工程的内存布局（F103C8T6 一共只有 20 KB RAM，每一块都要说清楚）：
 *
 *     0x20000000  ┌──────────────────────────────┐
 *                 │ .data   有初值的全局变量        │
 *                 │ .bss    无初值的全局变量        │
 *                 │         （含 FreeRTOS 的 9 KB 堆 │
 *                 │          ucHeap 数组、静态缓冲） │
 *                 ├──────────────────────────────┤
 *                 │ _Min_Heap_Size  0x200 (512 B)  │ ← C 库 malloc 用（本工程基本不用）
 *                 ├──────────────────────────────┤
 *                 │ _Min_Stack_Size 0x400 (1 KB)   │ ← 主栈 MSP
 *                 │  只用于：复位到调度器启动前、    │
 *                 │          以及所有中断服务程序    │
 * 0x20005000      └──────────────────────────────┘
 *
 *   ★ 任务栈【不在】这里 —— 它们是 FreeRTOS 用 pvPortMalloc 从
 *     ucHeap（在 .bss 里）分出来的。所以任务栈给多了会挤占 .bss，
 *     表现为"全局变量莫名其妙被改"或"pvPortMalloc 失败"。
 *
 *   ★ 因此校验 RAM 是否够用的方法不是看编译器报的 .bss 大小，
 *     而是看链接器有没有报 "region RAM overflowed"，
 *     以及 Map 文件里 ._user_heap_stack 段结束后剩了多少。
 * ============================================================ */

/* Entry Point */
ENTRY(Reset_Handler)

/* 用户模式栈的最高地址（栈向下生长） */
_estack = ORIGIN(RAM) + LENGTH(RAM);

_Min_Heap_Size  = 0x200;   /* 512 B  */
_Min_Stack_Size = 0x400;   /* 1 KB   */

MEMORY
{
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 20K
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
}

SECTIONS
{
  /* ---------- 中断向量表：必须是 Flash 的第一个字节 ----------
   * Cortex-M3 在复位时从 0x08000000 取 MSP 初值、从 0x08000004 取 PC。
   * 所以这个段的位置是被硬件钉死的，不能挪。 */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* ---------- 代码 ---------- */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)         /* ARM 与 Thumb 互调用的胶水代码 */
    *(.glue_7t)
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* ---------- 常量 ---------- */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { . = ALIGN(4); *(.ARM.extab* .gnu.linkonce.armextab.*) . = ALIGN(4); } >FLASH
  .ARM :        { . = ALIGN(4); __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; . = ALIGN(4); } >FLASH

  .preinit_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  /* .data 段的初值放在 Flash 里，上电时由启动代码搬到 RAM */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* ---------- 栈与堆的保留区 ----------
   * 这个段本身不存数据，只是"占位"，用来让链接器检查
   * "RAM 到底还剩多少"。真正使用栈/堆的是运行时的代码。 */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
