/* demo.c - what a firmware crash investigation looks like with dbg_heap */
#include <stdio.h>
#include <string.h>
#include "dbg_heap.h"

static _Alignas(16) unsigned char arena[2048];

static void print_report(const char *when)
{
    heap_report_t r = heap_check();
    static const char *names[] = { "OK", "HEADER/UNDERFLOW", "OVERFLOW (tail canary)",
                                   "BAD SIZE", "DOUBLE FREE", "INVALID PTR" };
    printf("%-26s heap_check -> %s", when, names[r.status]);
    if (r.status != HEAP_OK)
        printf("  | block @+%zu allocated at %s:%d", r.block_offset,
               r.owner_file, r.owner_line);
    printf("\n");
}

static void leak(const void *p, size_t sz, const char *f, int l, void *ctx)
{
    (void)p; (void)ctx;
    printf("  LEAK %4zu bytes  <- %s:%d\n", sz, f, l);
}

int main(void)
{
    heap_init(arena, sizeof arena);

    char *uart_rx  = HEAP_MALLOC(64);
    char *gps_buf  = HEAP_MALLOC(32);
    char *log_line = HEAP_MALLOC(48);
    print_report("after 3 allocations:");

    strcpy(gps_buf, "$GPGGA,123519,4807.038,N,01131.000,E*47");   /* 40 bytes into 32 */
    print_report("after bad strcpy:");

    printf("  HEAP_FREE(gps_buf) -> %s\n",
           HEAP_FREE(gps_buf) == HEAP_ERR_TAIL_CANARY
               ? "refused, block kept for crash dump" : "freed");

    printf("  HEAP_FREE(uart_rx) twice -> ");
    HEAP_FREE(uart_rx);
    printf("%s\n", HEAP_FREE(uart_rx) == HEAP_ERR_DOUBLE_FREE ? "DOUBLE FREE caught" : "?");

    heap_stats_t s;
    heap_get_stats(&s);
    printf("\nstats: used=%zu peak=%zu free=%zu largest_free=%zu live=%zu\n",
           s.used_bytes, s.peak_used_bytes, s.free_bytes, s.largest_free, s.alloc_count);
    printf("live allocations at shutdown:\n");
    heap_walk_allocated(leak, NULL);
    (void)log_line;
    return 0;
}
