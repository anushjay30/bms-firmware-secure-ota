/*
 * bootloader.ld.S - Linker script for the bootloader (MPS2-AN385).
 *
 * Preprocessed like the application's script, so both use memory_map.h.
 * Code and constants live in BOOT (64 KB at 0x0, where the core boots from;
 * the flash HAL refuses to write there). Variables and the stack use RAM -
 * the same RAM the application initialises for itself when it starts, which
 * is fine: nothing the bootloader leaves there is needed afterwards. The one
 * thing passed on, the boot-info mailbox, is in SHARED, outside this script.
 */
#include "memory_map.h"

MEMORY
{
    BOOT (rx)  : ORIGIN = MM_BOOT_BASE, LENGTH = MM_BOOT_SIZE
    RAM  (rwx) : ORIGIN = MM_RAM_BASE,  LENGTH = MM_RAM_SIZE
}

ENTRY(boot_reset_handler)

SECTIONS
{
    .vectors :
    {
        KEEP(*(.boot_vectors))
    } > BOOT

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > BOOT

    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > BOOT

    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > BOOT
    __data_load = LOADADDR(.data);

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* No heap: nothing in the bootloader allocates (no printf, no malloc). */
    __stack_top   = ORIGIN(RAM) + LENGTH(RAM);
    __stack_limit = __stack_top - MM_BOOT_STACK_SIZE;

    /* End of the bootloader image in BOOT (the size report uses it). */
    __boot_end = __data_load + SIZEOF(.data);
}

ASSERT(ORIGIN(BOOT) == 0, "the core boots from address 0: the bootloader must start there")
ASSERT(__bss_end <= __stack_limit, "bootloader RAM overflow: .data/.bss collide with the stack")
