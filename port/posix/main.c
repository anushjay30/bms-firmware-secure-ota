/*
 * main.c - POSIX simulator entry point.
 *
 *   ./build/bms_posix [can-interface]     (default vcan0)
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "bms_app.h"
#include "bms_log.h"

int main(int argc, char **argv)
{
    const char *channel = (argc > 1) ? argv[1] : "vcan0";

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: %s [can-interface]   (default vcan0)\n", argv[0]);
        return 0;
    }

    bms_log("MAIN", "BMS firmware starting: FreeRTOS %s, POSIX port, CAN '%s'", tskKERNEL_VERSION_NUMBER, channel);
    if (!bms_app_init(channel)) {
        return 1;
    }

    vTaskStartScheduler();

    /* Only reached if the scheduler could not start. */
    bms_log("MAIN", "scheduler exited");
    return 1;
}
