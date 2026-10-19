/*
 * bms_config.h - Every calibration value of the BMS in one place.
 *
 * In production these would live in a calibration area that can be tuned
 * per pack variant without recompiling logic. Keep the plain
 * "#define NAME value" form: sim/tests/conftest.py parses this file so the
 * scenario tests derive their timing budgets from the same numbers.
 */
#ifndef BMS_CONFIG_H
#define BMS_CONFIG_H

/* ---- Cell voltage limits (Li-ion NMC), mV ------------------------------ */
#define BMS_CELL_OV_MV                4250
#define BMS_CELL_OV_HYST_MV           100   /* must fall below 4150 mV to clear */
#define BMS_CELL_UV_MV                2800
#define BMS_CELL_UV_HYST_MV           200   /* must rise above 3000 mV to clear */

/* ---- Cell temperature limit, 0.1 degC ---------------------------------- */
#define BMS_CELL_OT_DC                600   /* 60.0 degC                        */
#define BMS_CELL_OT_HYST_DC           50    /* must cool below 55.0 degC        */

/* ---- Pack current limits, mA (+ = discharge, - = charge) --------------- */
#define BMS_PACK_OC_DISCHARGE_MA      300000
/* Charging limit is lower: high charge current causes lithium plating. */
#define BMS_PACK_OC_CHARGE_MA         150000
#define BMS_PACK_OC_HYST_MA           20000

/* ---- Debounce: consecutive monitor samples needed to set / clear --------
 * A single noisy ADC reading or EMI spike must not open the contactors
 * under load (that is itself a hazard and wears the contacts), but a real
 * fault must be caught within the fault-tolerant time. N samples at the
 * monitor period trades the two off. Worst-case reaction time is roughly
 * (N + 1) * BMS_MONITOR_PERIOD_MS plus CAN latency.                       */
#define BMS_DEBOUNCE_OV_SET           3
#define BMS_DEBOUNCE_UV_SET           3
#define BMS_DEBOUNCE_OT_SET           2     /* temperature moves slowly; 2 rejects single glitches */
#define BMS_DEBOUNCE_OC_SET           2
#define BMS_DEBOUNCE_MEAS_TIMEOUT_SET 1     /* the timeout itself is the filter */
#define BMS_DEBOUNCE_CLEAR            5     /* "condition gone" needs 5 good samples */

/* ---- Task timing, ms ---------------------------------------------------- */
#define BMS_MONITOR_PERIOD_MS         100
#define BMS_STATUS_PERIOD_MS          100
#define BMS_CONTACTOR_STEP_MS         10
#define BMS_SUPERVISOR_PERIOD_MS      50
#define BMS_CAN_RX_TIMEOUT_MS         50

/* ---- Measurement freshness --------------------------------------------- */
/* Simulator sends cells every 50 ms and pack data every 10 ms. */
#define BMS_MEAS_TIMEOUT_MS           300

/* ---- Precharge ----------------------------------------------------------
 * Closing straight onto the inverter's DC-link capacitor would draw a huge
 * inrush current that can weld the contactor tips. Instead the negative
 * contactor and a precharge relay (in series with a resistor) close first;
 * the link charges with time constant R*C. Once it is within a few percent
 * of pack voltage the main positive contactor closes with almost no
 * current. If the link never gets there (short circuit, failed resistor)
 * we must give up rather than cook the resistor.                         */
#define BMS_PRECHARGE_DONE_PCT        95
#define BMS_PRECHARGE_TIMEOUT_MS      500

/* ---- Software watchdog -------------------------------------------------- */
/* Every task loops at least every 100 ms, so 3 missed periods = stalled.   */
#define BMS_WDG_TIMEOUT_MS            300

/* ---- State of charge ---------------------------------------------------- */
#define BMS_PACK_CAPACITY_MAH         50000
#define BMS_SOC_INITIAL_PCT_X100      8000  /* 80.00 % */

/* ---- Firmware update ------------------------------------------------------
 * A new image runs "on trial" until it confirms itself; the bootloader rolls
 * back if it has not after a few boots. It confirms once every task has been
 * checking in and no fault has been latched or active for this long. A real
 * vehicle would want much more evidence (a drive cycle, a contactor
 * close/open, a VCU acknowledgement); 3 s keeps the tests quick.            */
#define BMS_CONFIRM_HEALTHY_MS        3000
/* The update task blocks for a whole signature check (~0.3 s on a 25 MHz
 * Cortex-M3), so its watchdog deadline is longer than the other tasks'.     */
#define BMS_OTA_WDG_TIMEOUT_MS        2000
/* An update session is abandoned after this long without a frame.          */
#define BMS_OTA_SESSION_TIMEOUT_MS    5000

/* ---- Build features ----------------------------------------------------- */
/* Test hooks let the scenario harness misbehave on purpose (e.g. stall a task
 * to prove the watchdog works). They must be compiled out of production
 * firmware; the POSIX simulator build turns them on from the Makefile. */
#ifndef BMS_ENABLE_TEST_HOOKS
#define BMS_ENABLE_TEST_HOOKS         0
#endif

#endif /* BMS_CONFIG_H */
