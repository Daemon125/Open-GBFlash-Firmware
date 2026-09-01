/* Flash: 0x0000 bootloader, 0x3E00 boot-info record (tools/mkimage.py), 0x4000
 * this image, 0x3E800 DataFlash. The 36-entry vector table must sit at the
 * image base; vector[0] must satisfy (sp & 0x2FFE0000) == 0x20000000. */

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00004000, LENGTH = 231424
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32768
}

__stack_top   = 0x20008000;       /* vector[0] */
__app_base    = 0x00004000;

/* Update-request word: bl_boot() clears it, the application writes it to ask
 * for an update. Anything initialised over it boots into update mode. */
__bl_magic    = 0x20000090;

SECTIONS
{
  .vectors 0x00004000 : ALIGN(4)
  {
    __vectors_start = .;
    KEEP(*(.vectors))
    KEEP(*(.vectors.*))
    __vectors_end = .;
  } > FLASH

  ASSERT(__vectors_start == 0x00004000,
         "firmware.ld: .vectors must start at 0x00004000")
  ASSERT(__vectors_end - __vectors_start == 0x90,
         "firmware.ld: vector table must be exactly 0x90 bytes (36 entries)")

  .text : ALIGN(4)
  {
    __text_start = .;
    *(.text.Reset_Handler)
    *(.text .text.* .gnu.linkonce.t.*)
    . = ALIGN(4);
    __text_end = .;
  } > FLASH

  .rodata : ALIGN(4)
  {
    __rodata_start = .;
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    . = ALIGN(4);
    __rodata_end = .;
  } > FLASH

  /* Empty under -fno-unwind-tables; kept so a stray entry shows up as size. */
  .ARM.extab : ALIGN(4) { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
  .ARM.exidx : ALIGN(4)
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } > FLASH

  /* Must stay ahead of .bl_lowram: ALIGN(4) reads the current location
   * counter, which is the FLASH one only until a section switches regions. */
  __data_load = ALIGN(4);

  /* The official bootloader dispatches every exception as
   * `ldr r0,[0x20000000 + IPSR*4]; blx r0`, no range or Thumb check, so
   * anything allocated below 0x20000090 is branched to as a handler. */
  .bl_lowram 0x20000000 (NOLOAD) : ALIGN(4)
  {
    __bl_vectors_start = .;
    . += 0x90;
    __bl_vectors_end = .;

    __bl_magic_start = .;
    . += 4;
    __bl_magic_end = .;

    . += 12;                    /* to 0x200000A0 */
    __bl_lowram_end = .;
  } > RAM

  .data : AT (__data_load) ALIGN(4)
  {
    __data_start = .;
    *(.data .data.* .gnu.linkonce.d.*)
    . = ALIGN(4);
    __data_end = .;
  } > RAM

  .bss (NOLOAD) : ALIGN(4)
  {
    __bss_start = .;
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM

  /* Goes in the boot-info record's length field. */
  __image_end = __data_load + (__data_end - __data_start);
  __image_size = __image_end - __app_base;

  /* Cap is bl_official 0x1D98, "app_len err !"; the reconstruction allows more,
   * so a larger image fails only on retail boards. */
  ASSERT(__image_size <= 0x38800,
         "firmware.ld: image exceeds the 0x38800 budget the official bootloader enforces")

  /* APP_MIN_LEN in mkimage.py; the official bootloader has no minimum. */
  ASSERT(__image_size >= 0x90,
         "firmware.ld: image is shorter than a vector table")

  .stack (NOLOAD) : ALIGN(8)
  {
    __heap_end = .;
  } > RAM

  ASSERT(__heap_end <= __stack_top - 1024,
         "firmware.ld: less than 1 KB of stack headroom left")

  ASSERT(__bl_vectors_start == 0x20000000,
         "firmware.ld: .bl_lowram is not at the official bootloader's SRAM vector base")
  ASSERT(__bl_vectors_end - __bl_vectors_start == 0x90,
         "firmware.ld: the reserved SRAM vector table is not 36 entries")
  ASSERT(__bl_magic_start == 0x20000090,
         "firmware.ld: .bl_magic is not at the address the bootloader reads")
  ASSERT(__bl_lowram_end == 0x200000A0,
         "firmware.ld: low RAM does not end where the vendor application's RW begins")
  ASSERT(__data_start >= 0x200000A0,
         "firmware.ld: .data runs over the official bootloader's SRAM vector table")
  ASSERT(__data_end <= __bl_magic || __data_start > __bl_magic,
         "firmware.ld: .data overlaps the bootloader magic word")
  ASSERT(__bss_start >= __bl_magic_end || __bss_end <= __bl_magic,
         "firmware.ld: .bss overlaps the bootloader magic word")

  /DISCARD/ : { *(.comment) *(.note.*) *(.ARM.attributes) }
}
