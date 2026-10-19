/*
 * bms_app.h - Application entry point used by every port's main().
 */
#ifndef BMS_APP_H
#define BMS_APP_H

#include <stdbool.h>

/* Open the CAN channel and statically create every queue, mutex and task.
 * Call once, before vTaskStartScheduler(). Returns false on failure. */
bool bms_app_init(const char *can_channel);

#endif /* BMS_APP_H */
