/* TXW8301 Simulator — CH32V203C8T6 linker script (64K Flash / 20K RAM) */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
  /* ★ 必须是 0x00000000（2026-09-21 上机实测，别再改回 0x08000000）：
   *   CH32V203 的**取指**走低地址区；链接到 0x08000000 的镜像烧进去
   *   **一条指令都不执行**（灯不亮、串口全静默），而同一份源码链接到
   *   0x00000000 就正常跑。证据：同一份源码只差链接基址的对照 ——
   *     firmware/build/probe1b.bin（0x0）     → 两灯闪 + COM23 正常打印
   *     firmware/build/probe1.bin（0x08000000）→ 一动不动
   *   且 WCH 自己的 EVT/EXAM/SRC/Ld/Link.ld 三个变体（D6/D8/D8W）
   *   也**全部**是 ORIGIN = 0x00000000。 */
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 64K
  RAM  (xrw)  : ORIGIN = 0x20000000, LENGTH = 20K
}

/* Top of user stack (full-descending) */
_eusrstack = ORIGIN(RAM) + LENGTH(RAM);

/* Small-data global pointer (used by -msmall-data-limit if enabled) */
__global_pointer$ = ORIGIN(RAM) + 0x800;

SECTIONS
{
  /* Reset entry
   * ★ 2026-09-21 实测：这里**只放** `.init` —— `.init` 必须正好 4 字节，
   *   中断向量表才能紧接在 0x4（与 WCH `EVT/EXAM/SRC/Ld/Link.ld` 一致）。
   *   曾经多 KEEP 了 `.text.entry`（+2 B）并把表 ALIGN 到 0x40，结果**中断全部不响应**。*/
  .init :
  {
    KEEP(*(.init))
    . = ALIGN(4);           /* ★ 让后面的向量表 4 字节对齐（32 位表项必须）*/
  } > FLASH

  /* Interrupt vector table：★ **紧接 `.init`**（= 0x8）—— 别再 ALIGN(64)。*/
  .vector :
  {
    KEEP(*(.vector))
  } > FLASH

  .text :
  {
    *(.text)
    *(.text.*)
    *(.gnu.linkonce.t.*)
  } > FLASH

  .rodata :
  {
    *(.rodata)
    *(.rodata.*)
    *(.srodata*)
    *(.gnu.linkonce.r.*)
  } > FLASH

  /* Initialized data: load in flash, run in RAM */
  .data :
  {
    . = ALIGN(4);
    _data_vma = .;
    *(.data)
    *(.data.*)
    *(.sdata*)
    *(.gnu.linkonce.d.*)
    _edata = .;
  } > RAM AT > FLASH
  _data_lma = LOADADDR(.data);

  /* Zero-init data */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    *(.bss)
    *(.bss.*)
    *(.sbss*)
    *(COMMON)
    _ebss = .;
  } > RAM

  . = ALIGN(4);
  _end = .;

  /* Heap (not used by default; provided for libc compatibility) */
  .heap (NOLOAD) :
  {
    . = ALIGN(4);
    _heap_start = .;
    . = ORIGIN(RAM) + LENGTH(RAM) - 256;
    _heap_end = .;
  } > RAM
}
