/**
 * @file steribox_app.h
 * SteriBox UV Sterilizer - application logic on top of the SquareLine UI.
 *
 * Portable: runs unchanged on the PC simulator and on the Elecrow panel.
 * Call steribox_app_init() once, right after ui_init().
 */
#ifndef STERIBOX_APP_H
#define STERIBOX_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/** Sterilization cycle state */
typedef enum {
    SBX_STATE_IDLE = 0,
    SBX_STATE_WARMUP,        /* 3 s safety delay before the lamps energise */
    SBX_STATE_RUNNING,
    SBX_STATE_PAUSED_DOOR,   /* door opened mid-cycle: frozen, press to resume */
    SBX_STATE_DONE,
    SBX_STATE_ABORTED_DOOR,
} sbx_state_t;

/** How a cycle ended. EVERY cycle gets one of these - a cycle that was
 *  stopped or timed out on an open door is still a recorded cycle with a
 *  full ledger row and a printable ticket, it simply did not complete. */
typedef enum {
    SBX_END_COMPLETED = 0,   /* ran to the selected duration              */
    SBX_END_OPERATOR_STOP,   /* STOP pressed during preheat or run        */
    SBX_END_DOOR_TIMEOUT,    /* door left open past the 10 s pause window */
} sbx_end_reason_t;

/** Attach events/timers to the generated UI. Call after ui_init(). */
void steribox_app_init(void);

/** Current cycle state (for tests / target code) */
sbx_state_t steribox_app_get_state(void);

/*Backends for the SquareLine named event hooks (called by ui_events.c)*/
void steribox_ev_confirm_pwd(void);
void steribox_ev_cancel_pwd(void);
void steribox_ev_save_config(void);

/* Info screen — Export / Print action callbacks (LVGL event handlers) */
void sbx_info_export_cb(lv_event_t * e);
void sbx_info_print_cb(lv_event_t * e);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*STERIBOX_APP_H*/
