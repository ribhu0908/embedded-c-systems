/*
 * sw_timer.c - see include/sw_timer.h
 *
 * Example list for timers due at absolute ticks 5, 8, 8, 20:
 *
 *   head -> [5] -> [3] -> [0] -> [12] -> NULL
 *
 * On a real target, wrap list mutation in a critical section (or run
 * sw_timer_tick from a task instead of the ISR). Kept out of this file so it
 * builds and tests on a host.
 */
#include "sw_timer.h"

#include <stddef.h>

static sw_timer_t *g_head;

void sw_timer_init(sw_timer_t *t, sw_timer_cb cb, void *arg)
{
    t->next   = NULL;
    t->delta  = 0;
    t->period = 0;
    t->cb     = cb;
    t->arg    = arg;
    t->active = false;
}

static void unlink_timer(sw_timer_t *t)
{
    sw_timer_t **pp = &g_head;
    while (*pp != NULL && *pp != t)
        pp = &(*pp)->next;
    if (*pp == NULL)
        return;
    if (t->next != NULL)
        t->next->delta += t->delta;     /* successor keeps its absolute time */
    *pp = t->next;
    t->next = NULL;
    t->active = false;
}

static void insert_timer(sw_timer_t *t, uint32_t ticks)
{
    sw_timer_t **pp = &g_head;

    /* Walk while the new timer expires strictly later, so equal expiries
       fire in the order they were started (FIFO). */
    while (*pp != NULL && ticks >= (*pp)->delta) {
        ticks -= (*pp)->delta;
        pp = &(*pp)->next;
    }
    t->delta = ticks;
    t->next  = *pp;
    if (t->next != NULL)
        t->next->delta -= ticks;
    *pp = t;
    t->active = true;
}

int sw_timer_start(sw_timer_t *t, uint32_t ticks, uint32_t period)
{
    if (t == NULL || t->cb == NULL || ticks == 0)
        return -1;
    if (t->active)
        unlink_timer(t);
    t->period = period;
    insert_timer(t, ticks);
    return 0;
}

void sw_timer_stop(sw_timer_t *t)
{
    if (t != NULL && t->active)
        unlink_timer(t);
}

uint32_t sw_timer_remaining(const sw_timer_t *t)
{
    uint32_t sum = 0;
    for (const sw_timer_t *n = g_head; n != NULL; n = n->next) {
        sum += n->delta;
        if (n == t)
            return sum;
    }
    return 0;
}

void sw_timer_tick(void)
{
    if (g_head == NULL)
        return;
    if (g_head->delta > 0)
        g_head->delta--;

    /* Pop everything due now. Callbacks may restart or stop any timer,
       so the list is re-read from g_head after every callback. */
    while (g_head != NULL && g_head->delta == 0) {
        sw_timer_t *t = g_head;
        g_head = t->next;
        t->next = NULL;
        t->active = false;

        if (t->period > 0)
            insert_timer(t, t->period);
        t->cb(t, t->arg);
    }
}

uint32_t sw_timer_next_expiry(void)
{
    return g_head ? g_head->delta : UINT32_MAX;
}

void sw_timer_reset_all(void)
{
    while (g_head != NULL) {
        sw_timer_t *t = g_head;
        g_head = t->next;
        t->next = NULL;
        t->active = false;
    }
}
