/*
 * bms_types.h - Types shared by the BMS application, its CAN codec and the ports.
 *
 * Kept free of any OS or compiler-specific dependency so it can be included
 * from host unit tests, the FreeRTOS POSIX build and (later) the Cortex-M3 build.
 */
#ifndef BMS_TYPES_H
#define BMS_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* Pack topology: 12 cells in series (12S1P). */
#define BMS_NUM_CELLS 12u

/* Contactor state machine states. Values are transmitted on CAN (BMS_Status.State). */
typedef enum {
    BMS_STATE_OPEN      = 0, /* all contactors open, HV bus isolated            */
    BMS_STATE_PRECHARGE = 1, /* negative + precharge relay closed, bus charging  */
    BMS_STATE_CLOSED    = 2, /* negative + positive closed, pack connected       */
    BMS_STATE_FAULT     = 3  /* latched fault, all open until an explicit reset  */
} bms_state_t;

/* Fault identifiers. Bit positions in the FaultsLatched/FaultsActive bitfields. */
typedef enum {
    BMS_FAULT_CELL_OV      = 0, /* cell overvoltage                          */
    BMS_FAULT_CELL_UV      = 1, /* cell undervoltage                         */
    BMS_FAULT_CELL_OT      = 2, /* cell overtemperature                      */
    BMS_FAULT_PACK_OC      = 3, /* pack overcurrent (charge or discharge)    */
    BMS_FAULT_MEAS_TIMEOUT = 4, /* measurement data stopped arriving         */
    BMS_FAULT_PRECHARGE    = 5, /* DC link did not reach target in time      */
    BMS_FAULT_WATCHDOG     = 6, /* a firmware task stopped checking in       */
    BMS_FAULT_COUNT
} bms_fault_id_t;

#define BMS_FAULT_BIT(id) ((uint8_t)(1u << (unsigned)(id)))

/* Commands accepted from the vehicle controller (VCU_Command.Command). */
typedef enum {
    BMS_CMD_NONE  = 0, /* also used internally as "re-evaluate now"  */
    BMS_CMD_CLOSE = 1, /* request OPEN -> PRECHARGE -> CLOSED         */
    BMS_CMD_OPEN  = 2, /* request contactors open                     */
    BMS_CMD_RESET = 3  /* request fault reset (only if condition gone) */
} bms_cmd_t;

/*
 * One coherent snapshot of every measurement, produced by the monitor task
 * every BMS_MONITOR_PERIOD_MS and consumed by fault detection.
 * Units are fixed-point integers (mV, 0.1 degC, mA) on purpose: the Cortex-M3
 * target has no FPU, so floats would pull in slow soft-float library code.
 */
typedef struct {
    uint32_t t_ms;                       /* sample time, ms since boot          */
    uint16_t cell_mv[BMS_NUM_CELLS];     /* cell voltage, mV                    */
    int16_t  cell_dc[BMS_NUM_CELLS];     /* cell temperature, 0.1 degC          */
    int32_t  pack_ma;                    /* pack current, mA, + = discharge     */
    uint32_t pack_mv;                    /* sum of cell voltages, mV            */
    uint16_t link_mv;                    /* DC-link (inverter side) voltage, mV */
    bool     valid;                      /* every signal received and fresh     */
    bool     stale;                      /* was valid before, now timed out     */
} bms_sample_t;

/* Details of a newly latched fault, sent as BMS_FaultEvent. */
typedef struct {
    bms_fault_id_t id;
    uint8_t        cell;  /* 1-based cell number, 0 = not cell specific      */
    int32_t        value; /* offending value: mV, 0.1 degC, mA or a bitmask   */
} bms_fault_event_t;

#endif /* BMS_TYPES_H */
