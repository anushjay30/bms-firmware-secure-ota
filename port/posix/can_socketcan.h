/*
 * can_socketcan.h - POSIX-port-only hooks of the SocketCAN driver.
 */
#ifndef CAN_SOCKETCAN_H
#define CAN_SOCKETCAN_H

/* Called from vApplicationTickHook() (interrupt context). */
void can_socketcan_tick_hook(void);

#endif /* CAN_SOCKETCAN_H */
