/*
 * mps2_an385.ld.S - Linker script for the BMS application on MPS2-AN385.
 *
 * Run through the C preprocessor by the Makefile (-> build/qemu/link.ld) so
 * it can include memory_map.h, the same header the C code uses.
 *
 * The application executes in place from its slot, so it is linked twice:
 * once for Slot A and once for Slot B (BMS_APP_SLOT_B). Slot layout:
 *   +0x000  image header  (256 bytes, see bms_image.h)
 *   +0x100  vector table  (VTOR points here)
 *   ...     code, read-only data, initial values of .data
 * The bootloader is a separate image (bootloader/bootloader.ld.S).
 */
#include "memory_map.h"

MEMORY
{
    BOOT   (rx)  : ORIGIN = MM_BOOT_BASE,   LENGTH = MM_BOOT_SIZE
    META   (r)   : ORIGIN = MM_META_BASE,   LENGTH = MM_META_SIZE
    SLOT_A (rx)  : ORIGIN = MM_SLOT_A_BASE, LENGTH = MM_SLOT_SIZE
    SLOT_B (rx)  : ORIGIN = MM_SLOT_B_BASE, LENGTH = MM_SLOT_SIZE
    RAM    (rwx) : ORIGIN = MM_RAM_BASE,    LENGTH = MM_RAM_SIZE
    SHARED (rw)  : ORIGIN = MM_SHARED_BASE, LENGTH = MM_SHARED_SIZE
}

#if defined(BMS_APP_SLOT_B)
REGION_ALIAS("APP", SLOT_B);
#else
REGION_ALIAS("APP", SLOT_A);
#endif

ENTRY(Reset_Handler)

SECTIONS
{
    .image_header :
    {
        KEEP(*(.image_header))
        . = MM_IMAGE_HEADER_SIZE;   /* pad: the vector table starts at +0x100 */
    } > APP

    .isr_vector :
    {
        __app_vectors = .;
        KEEP(*(.isr_vector))
    } > APP

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > APP

    /* Unwind tables (only referenced if something unwinds; kept for safety). */
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > APP

    /* .data lives in RAM (VMA) but its initial values are stored in the slot
     * (LMA); Reset_Handler copies them across. Real RAM powers up random. */
    .data : ALIGN(4)
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > APP
    __data_load = LOADADDR(.data);

    /* Zero-initialised data: no bytes in the image, Reset_Handler clears it. */
    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /* There is deliberately no heap. _sbrk() (fault.c) refuses to grow one,
     * so any malloc - e.g. hidden inside a C library call - fails loudly. */
    __heap_start = __bss_end;

    /* Main stack (MSP) at the very top of RAM, growing down. The hardware
     * loads its initial value from vector table entry 0 at reset. */
    __msp_stack_top   = ORIGIN(RAM) + LENGTH(RAM);
    __msp_stack_limit = __msp_stack_top - MM_MSP_STACK_SIZE;

    /* SHARED (the bootloader -> app mailbox) gets no section on purpose: the
     * code reaches it through a fixed address (boot_info.h), so no ELF
     * segment covers it and no loader ever zeroes or reloads it. */

    /* First address after the image in its slot, and the size of everything
     * after the header (recorded in the header, covered by the signature). */
    __image_end  = __data_load + SIZEOF(.data);
    __image_size = __image_end - __app_vectors;
}

ASSERT(__app_vectors == MM_APP_VECTORS, "vector table must follow the 256-byte image header")
ASSERT((__app_vectors & 0xFF) == 0, "VTOR needs the 48-entry table aligned to 256 bytes")
ASSERT(__bss_end <= __msp_stack_limit, "RAM overflow: .data/.bss collide with the MSP stack")
