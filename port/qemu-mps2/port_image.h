/*
 * port_image.h - What this application image knows about itself.
 */
#ifndef PORT_IMAGE_H
#define PORT_IMAGE_H

#include "bms_image.h"

extern const bms_image_header_t g_image_header;

/* The header as stored in flash (signed, with the release version). */
const volatile bms_image_header_t *port_running_image(void);

/* 'A' or 'B': the slot this build was linked for, and therefore runs from. */
char port_running_slot_name(void);

#endif /* PORT_IMAGE_H */
