/*
 * boot_jump.h - Hand the CPU over to an application image.
 */
#ifndef BOOT_JUMP_H
#define BOOT_JUMP_H

#include <stdint.h>

/* Start the application whose vector table is at vector_table (slot + 0x100).
 * The caller must already have verified the image. Does not return. */
void boot_jump_to_app(uint32_t vector_table) __attribute__((noreturn));

#endif /* BOOT_JUMP_H */
