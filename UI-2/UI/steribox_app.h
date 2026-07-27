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

/** Attach events/timers to the generated UI. Call after ui_init(). */
void steribox_app_init(void);

/** Current cycle state (for tests / target code) */
sbx_state_t steribox_app_get_state(void);

/*Backends for the SquareLine named event hooks (called by ui_events.c)*/
void steribox_ev_confirm_pwd(void);
void steribox_ev_cancel_pwd(void);
void steribox_ev_save_config(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*STERIBOX_APP_H*/
