/*
 * sw_timer.h - Software timers on a delta list, driven by one hardware tick.
 *
 * Same idea most RTOS kernels use: timers are kept sorted by expiry and each
 * node stores ticks *relative to the previous node*. The tick ISR therefore
 * only decrements the head - O(1) per tick no matter how many timers exist.
 * Insert/stop are O(n), which is the right trade-off when ticks are frequent
 * and timer changes are rare.
 *
 * No dynamic memory: the caller owns every sw_timer_t (static or on a stack
 * that outlives the timer).
 */
#ifndef SW_TIMER_H
#define SW_TIMER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct sw_timer sw_timer_t;
typedef void (*sw_timer_cb)(sw_timer_t *t, void *arg);

struct sw_timer {
    sw_timer_t  *next;
    uint32_t     delta;      /* ticks after the previous node expires   */
    uint32_t     period;     /* 0 = one-shot, else auto-reload interval */
    sw_timer_cb  cb;
    void        *arg;
    bool         active;
};

void sw_timer_init(sw_timer_t *t, sw_timer_cb cb, void *arg);

/* Arm (or re-arm) to fire after `ticks`, repeating every `period` if > 0. */
int  sw_timer_start(sw_timer_t *t, uint32_t ticks, uint32_t period);

/* Disarm; remaining time folds into the next node so others are unaffected. */
void sw_timer_stop(sw_timer_t *t);

/* Ticks until t fires, or 0 if inactive. */
uint32_t sw_timer_remaining(const sw_timer_t *t);

/* Call once per hardware tick. Fires every expired callback, in order. */
void sw_timer_tick(void);

/* For tickless idle: ticks until the next expiry (UINT32_MAX if none). */
uint32_t sw_timer_next_expiry(void);

void sw_timer_reset_all(void);   /* test helper */

#endif /* SW_TIMER_H */
