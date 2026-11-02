/*
 * Host unit tests. No framework dependency on purpose - the same file can be
 * cross-compiled and run on a dev board with printf over UART.
 */
#include <stdio.h>
#include <string.h>

#include "dbg_heap.h"
#include "ringbuf.h"
#include "sw_timer.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                  \
    } while (0)

#define RUN(fn)                     \
    do {                            \
        printf("[ RUN  ] %s\n", #fn); \
        fn();                       \
    } while (0)

/* ------------------------------------------------------------------ heap */
static _Alignas(16) unsigned char arena[4096];

static void test_heap_alloc_free_reuse(void)
{
    CHECK(heap_init(arena, sizeof arena) == 0);
    void *a = HEAP_MALLOC(100);
    void *b = HEAP_MALLOC(200);
    CHECK(a && b && a != b);
    memset(a, 0xAA, 100);
    memset(b, 0xBB, 200);
    CHECK(heap_check().status == HEAP_OK);

    CHECK(HEAP_FREE(a) == HEAP_OK);
    void *c = HEAP_MALLOC(64);
    CHECK(c == a);                       /* first fit reuses the hole */
    CHECK(HEAP_FREE(b) == HEAP_OK);
    CHECK(HEAP_FREE(c) == HEAP_OK);

    heap_stats_t s;
    heap_get_stats(&s);
    CHECK(s.alloc_count == 0);
    CHECK(s.free_blocks == 1);           /* fully coalesced */
    CHECK(s.peak_used_bytes >= 300);
}

static void test_heap_detects_overflow_and_reports_owner(void)
{
    heap_init(arena, sizeof arena);
    char *p = HEAP_MALLOC(32);
    int alloc_line = __LINE__ - 1;
    (void)HEAP_MALLOC(32);

    p[32] = 'X';                         /* one-byte overflow */
    heap_report_t r = heap_check();
    CHECK(r.status == HEAP_ERR_TAIL_CANARY);
    CHECK(r.owner_line == alloc_line);
    CHECK(r.owner_file && strstr(r.owner_file, "test_main.c"));
    CHECK(HEAP_FREE(p) == HEAP_ERR_TAIL_CANARY);
}

static void test_heap_detects_double_free_and_bad_ptr(void)
{
    heap_init(arena, sizeof arena);
    char *p = HEAP_MALLOC(16);
    (void)HEAP_MALLOC(16);               /* keep p from merging away */
    CHECK(HEAP_FREE(p) == HEAP_OK);
    CHECK(HEAP_FREE(p) == HEAP_ERR_DOUBLE_FREE);

    int local;
    CHECK(HEAP_FREE(&local) == HEAP_ERR_INVALID_PTR);
    char *q = HEAP_MALLOC(16);
    CHECK(HEAP_FREE(q + 1) == HEAP_ERR_INVALID_PTR);   /* interior pointer */
    CHECK(HEAP_FREE(NULL) == HEAP_OK);
}

static void test_heap_detects_header_smash(void)
{
    heap_init(arena, sizeof arena);
    char *a = HEAP_MALLOC(16);
    int alloc_line = __LINE__ - 1;
    (void)HEAP_MALLOC(16);
    a[-1] = 0;                           /* one-byte underflow */
    heap_report_t r = heap_check();
    CHECK(r.status == HEAP_ERR_HEADER_MAGIC);
    CHECK(r.owner_line == alloc_line);   /* guard caught it before owner info */
    CHECK(HEAP_FREE(a) == HEAP_ERR_HEADER_MAGIC);
}

static void test_heap_exhaustion_and_alignment(void)
{
    heap_init(arena, sizeof arena);
    int n = 0;
    void *p;
    while ((p = HEAP_MALLOC(100)) != NULL) {
        CHECK(((uintptr_t)p % _Alignof(max_align_t)) == 0);
        n++;
    }
    CHECK(n > 10);
    CHECK(HEAP_MALLOC(0) == NULL);
    CHECK(HEAP_MALLOC(sizeof arena) == NULL);
    CHECK(heap_check().status == HEAP_OK);
}

static void leak_cb(const void *p, size_t sz, const char *f, int l, void *ctx)
{
    (void)p; (void)f; (void)l;
    *(size_t *)ctx += sz;
}

static void test_heap_leak_walk(void)
{
    heap_init(arena, sizeof arena);
    (void)HEAP_MALLOC(10);
    (void)HEAP_MALLOC(20);
    size_t leaked = 0;
    heap_walk_allocated(leak_cb, &leaked);
    CHECK(leaked == 30);
}

/* -------------------------------------------------------------- ringbuf */
static void test_rb_basic_and_wrap(void)
{
    uint8_t store[8];
    ringbuf_t rb;
    CHECK(rb_init(&rb, store, 6) == -1);          /* not a power of two */
    CHECK(rb_init(&rb, store, 8) == 0);

    CHECK(rb_write(&rb, "abcdef", 6) == 6);
    char out[16] = {0};
    CHECK(rb_read(&rb, out, 4) == 4 && memcmp(out, "abcd", 4) == 0);

    CHECK(rb_write(&rb, "123456", 6) == 6);       /* wraps around the end */
    CHECK(rb_used(&rb) == 8 && rb_free(&rb) == 0);
    CHECK(rb_write(&rb, "z", 1) == 0);            /* full, nothing lost */

    memset(out, 0, sizeof out);
    CHECK(rb_peek(&rb, out, 3) == 3 && rb_used(&rb) == 8);
    CHECK(rb_read(&rb, out, 16) == 8);
    CHECK(memcmp(out, "ef123456", 8) == 0);
    CHECK(rb_used(&rb) == 0);
}

static void test_rb_index_overflow(void)
{
    uint8_t store[4];
    ringbuf_t rb;
    rb_init(&rb, store, 4);
    /* Force head/tail near UINT32_MAX to prove free-running wrap works. */
    atomic_store(&rb.head, UINT32_MAX - 1);
    atomic_store(&rb.tail, UINT32_MAX - 1);
    CHECK(rb_write(&rb, "wxyz", 4) == 4);
    char out[4];
    CHECK(rb_read(&rb, out, 4) == 4 && memcmp(out, "wxyz", 4) == 0);
}

/* ------------------------------------------------------------- sw_timer */
static int  g_fired[8];
static int  g_order[16];
static int  g_order_n;
static void on_fire(sw_timer_t *t, void *arg)
{
    (void)t;
    int id = (int)(intptr_t)arg;
    g_fired[id]++;
    if (g_order_n < 16)
        g_order[g_order_n++] = id;
}

static void tick_n(int n)
{
    while (n--)
        sw_timer_tick();
}

static void test_timer_ordering_and_fifo(void)
{
    sw_timer_reset_all();
    memset(g_fired, 0, sizeof g_fired);
    g_order_n = 0;

    sw_timer_t t[4];
    for (int i = 0; i < 4; i++)
        sw_timer_init(&t[i], on_fire, (void *)(intptr_t)i);

    sw_timer_start(&t[0], 20, 0);
    sw_timer_start(&t[1], 5, 0);
    sw_timer_start(&t[2], 8, 0);
    sw_timer_start(&t[3], 8, 0);       /* same expiry as t[2] */
    CHECK(sw_timer_next_expiry() == 5);
    CHECK(sw_timer_remaining(&t[0]) == 20);

    tick_n(20);
    CHECK(g_order_n == 4);
    CHECK(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3 && g_order[3] == 0);
}

static void test_timer_stop_keeps_others_exact(void)
{
    sw_timer_reset_all();
    memset(g_fired, 0, sizeof g_fired);
    sw_timer_t a, b;
    sw_timer_init(&a, on_fire, (void *)0);
    sw_timer_init(&b, on_fire, (void *)1);
    sw_timer_start(&a, 10, 0);
    sw_timer_start(&b, 15, 0);
    tick_n(3);
    sw_timer_stop(&a);
    CHECK(sw_timer_remaining(&b) == 12);
    tick_n(11);
    CHECK(g_fired[1] == 0);
    tick_n(1);
    CHECK(g_fired[1] == 1 && g_fired[0] == 0);
}

static void test_timer_periodic(void)
{
    sw_timer_reset_all();
    memset(g_fired, 0, sizeof g_fired);
    sw_timer_t p;
    sw_timer_init(&p, on_fire, (void *)0);
    sw_timer_start(&p, 4, 4);
    tick_n(20);
    CHECK(g_fired[0] == 5);
    sw_timer_stop(&p);
    tick_n(20);
    CHECK(g_fired[0] == 5);
    CHECK(sw_timer_start(&p, 0, 0) == -1);
}

static sw_timer_t g_self;
static int g_self_count;
static void restart_self(sw_timer_t *t, void *arg)
{
    (void)arg;
    if (++g_self_count < 3)
        sw_timer_start(t, 2, 0);        /* re-arm from inside the callback */
}

static void test_timer_restart_from_callback(void)
{
    sw_timer_reset_all();
    g_self_count = 0;
    sw_timer_init(&g_self, restart_self, NULL);
    sw_timer_start(&g_self, 2, 0);
    tick_n(10);
    CHECK(g_self_count == 3);
    CHECK(sw_timer_next_expiry() == UINT32_MAX);
}

int main(void)
{
    RUN(test_heap_alloc_free_reuse);
    RUN(test_heap_detects_overflow_and_reports_owner);
    RUN(test_heap_detects_double_free_and_bad_ptr);
    RUN(test_heap_detects_header_smash);
    RUN(test_heap_exhaustion_and_alignment);
    RUN(test_heap_leak_walk);
    RUN(test_rb_basic_and_wrap);
    RUN(test_rb_index_overflow);
    RUN(test_timer_ordering_and_fifo);
    RUN(test_timer_stop_keeps_others_exact);
    RUN(test_timer_periodic);
    RUN(test_timer_restart_from_callback);

    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
