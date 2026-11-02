/*
 * dbg_heap.c - see include/dbg_heap.h
 *
 * Arena layout (blocks are contiguous, so the arena is its own block list):
 *
 *   +--------+----------------------+--------+   +--------+-- ...
 *   | header |  user payload (size) | canary |   | header |
 *   +--------+----------------------+--------+   +--------+-- ...
 *   ^ block start                    ^ 4-byte tail guard
 *
 * header.span = bytes from this header to the next header (always aligned),
 * so the next block is found by pointer arithmetic, never by a stored link
 * that a stray write could redirect.
 */
#include "dbg_heap.h"

#include <stdalign.h>
#include <string.h>

#define HDR_MAGIC_USED  0xA110CA7Eu
#define HDR_MAGIC_FREE  0xF7EEB10Cu
#define TAIL_CANARY     0xDEADC0DEu
#define ALIGN           (alignof(max_align_t))
#define ALIGN_UP(x)     (((x) + (ALIGN - 1)) & ~(size_t)(ALIGN - 1))

typedef struct block_hdr {
    uint32_t    magic;
    uint32_t    _pad;
    size_t      span;       /* header + payload + canary, rounded up      */
    size_t      size;       /* user-requested size (0 when free)          */
    const char *file;       /* owner: who allocated (or last freed) it    */
    int         line;
} block_hdr_t;

#define CANARY_SIZE     sizeof(uint32_t)
/* Header + a front guard word sitting right before the payload, so a
   buffer underflow hits the guard before it can reach size/owner fields. */
#define HDR_SIZE        ALIGN_UP(sizeof(block_hdr_t) + CANARY_SIZE)
#define FRONT_GUARD     0xF00DFACEu
#define MIN_SPAN        (HDR_SIZE + ALIGN_UP(1 + CANARY_SIZE))

static uint8_t *g_base;
static size_t   g_size;
static size_t   g_peak_used;

static inline block_hdr_t *first_block(void) { return (block_hdr_t *)g_base; }

static inline block_hdr_t *next_block(block_hdr_t *b)
{
    uint8_t *n = (uint8_t *)b + b->span;
    return (n < g_base + g_size) ? (block_hdr_t *)n : NULL;
}

static inline void *payload(block_hdr_t *b) { return (uint8_t *)b + HDR_SIZE; }

static inline void write_canary(block_hdr_t *b)
{
    uint32_t c = TAIL_CANARY, f = FRONT_GUARD;
    memcpy((uint8_t *)payload(b) + b->size, &c, CANARY_SIZE);
    memcpy((uint8_t *)payload(b) - CANARY_SIZE, &f, CANARY_SIZE);
}

static inline int front_ok(const block_hdr_t *b)
{
    uint32_t f;
    memcpy(&f, (const uint8_t *)b + HDR_SIZE - CANARY_SIZE, CANARY_SIZE);
    return f == FRONT_GUARD;
}

static inline int canary_ok(const block_hdr_t *b)
{
    uint32_t c;
    memcpy(&c, (const uint8_t *)b + HDR_SIZE + b->size, CANARY_SIZE);
    return c == TAIL_CANARY;
}

static inline int span_ok(const block_hdr_t *b)
{
    size_t off = (size_t)((const uint8_t *)b - g_base);
    if (b->span < MIN_SPAN || (b->span % ALIGN) != 0 || b->span > g_size - off)
        return 0;
    /* A used block's payload + canary must fit inside its own span,
       otherwise reading the canary would itself run off the block. */
    return b->size <= b->span - HDR_SIZE - CANARY_SIZE;
}

int heap_init(void *arena, size_t size)
{
    uintptr_t start = (uintptr_t)arena;
    uintptr_t aligned = (start + (ALIGN - 1)) & ~(uintptr_t)(ALIGN - 1);

    if (arena == NULL || size < (aligned - start) + MIN_SPAN)
        return -1;

    g_base = (uint8_t *)aligned;
    g_size = (size - (aligned - start)) & ~(size_t)(ALIGN - 1);
    g_peak_used = 0;

    block_hdr_t *b = first_block();
    b->magic = HDR_MAGIC_FREE;
    b->span  = g_size;
    b->size  = 0;
    b->file  = NULL;
    b->line  = 0;
    return 0;
}

/* Merge b with every free block that directly follows it. */
static void coalesce_forward(block_hdr_t *b)
{
    block_hdr_t *n;
    while ((n = next_block(b)) != NULL && n->magic == HDR_MAGIC_FREE)
        b->span += n->span;
}

void *heap_malloc_dbg(size_t size, const char *file, int line)
{
    if (g_base == NULL || size == 0 || size > g_size)
        return NULL;

    size_t need = HDR_SIZE + ALIGN_UP(size + CANARY_SIZE);

    for (block_hdr_t *b = first_block(); b != NULL; b = next_block(b)) {
        if (b->magic != HDR_MAGIC_FREE)
            continue;
        coalesce_forward(b);
        if (b->span < need)
            continue;

        /* Split only if the leftover can hold a minimum block. */
        if (b->span - need >= MIN_SPAN) {
            block_hdr_t *rest = (block_hdr_t *)((uint8_t *)b + need);
            rest->magic = HDR_MAGIC_FREE;
            rest->span  = b->span - need;
            rest->size  = 0;
            rest->file  = NULL;
            rest->line  = 0;
            b->span = need;
        }

        b->magic = HDR_MAGIC_USED;
        b->size  = size;
        b->file  = file;
        b->line  = line;
        write_canary(b);

        heap_stats_t s;
        heap_get_stats(&s);
        if (s.used_bytes > g_peak_used)
            g_peak_used = s.used_bytes;
        return payload(b);
    }
    return NULL;
}

/* Resolve a user pointer to its header, verifying it is a real block start. */
static block_hdr_t *owning_block(void *ptr)
{
    uint8_t *p = (uint8_t *)ptr;
    if (p < g_base + HDR_SIZE || p >= g_base + g_size)
        return NULL;
    for (block_hdr_t *b = first_block(); b != NULL; b = next_block(b)) {
        if (payload(b) == ptr)
            return b;
        if (!span_ok(b))
            return NULL;
    }
    return NULL;
}

heap_status_t heap_free_dbg(void *ptr, const char *file, int line)
{
    if (ptr == NULL)
        return HEAP_OK;                 /* free(NULL) is a no-op, like libc */

    block_hdr_t *b = owning_block(ptr);
    if (b == NULL)
        return HEAP_ERR_INVALID_PTR;
    if (b->magic == HDR_MAGIC_FREE)
        return HEAP_ERR_DOUBLE_FREE;
    if (b->magic != HDR_MAGIC_USED || !front_ok(b))
        return HEAP_ERR_HEADER_MAGIC;   /* underflow: block kept for forensics */
    if (!canary_ok(b))
        return HEAP_ERR_TAIL_CANARY;    /* block stays allocated for forensics */

    b->magic = HDR_MAGIC_FREE;
    b->size  = 0;
    b->file  = file;                    /* remember who freed it */
    b->line  = line;
    coalesce_forward(b);

    /* Merge into a free predecessor too, so fragmentation stats are honest. */
    block_hdr_t *prev = NULL;
    for (block_hdr_t *c = first_block(); c != NULL && c != b; c = next_block(c))
        prev = c;
    if (prev != NULL && prev->magic == HDR_MAGIC_FREE)
        prev->span += b->span;
    return HEAP_OK;
}

heap_report_t heap_check(void)
{
    heap_report_t r = { HEAP_OK, 0, NULL, 0 };

    for (block_hdr_t *b = first_block(); b != NULL; b = next_block(b)) {
        r.block_offset = (size_t)((uint8_t *)b - g_base);

        if (b->magic != HDR_MAGIC_USED && b->magic != HDR_MAGIC_FREE) {
            r.status = HEAP_ERR_HEADER_MAGIC;
            return r;                   /* header is garbage: owner unknown */
        }
        r.owner_file = b->file;
        r.owner_line = b->line;
        if (!span_ok(b)) {
            r.status = HEAP_ERR_BAD_SIZE;
            return r;
        }
        if (b->magic == HDR_MAGIC_USED && !front_ok(b)) {
            r.status = HEAP_ERR_HEADER_MAGIC;   /* owner still readable */
            return r;
        }
        if (b->magic == HDR_MAGIC_USED && !canary_ok(b)) {
            r.status = HEAP_ERR_TAIL_CANARY;
            return r;
        }
    }
    r.block_offset = 0;
    r.owner_file = NULL;
    r.owner_line = 0;
    return r;
}

void heap_get_stats(heap_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->total_bytes = g_size;

    for (block_hdr_t *b = first_block(); b != NULL; b = next_block(b)) {
        if (!span_ok(b))
            break;
        if (b->magic == HDR_MAGIC_USED) {
            out->used_bytes += b->size;
            out->alloc_count++;
        } else {
            size_t avail = b->span - HDR_SIZE - CANARY_SIZE;
            out->free_bytes += avail;
            out->free_blocks++;
            if (avail > out->largest_free)
                out->largest_free = avail;
        }
    }
    out->peak_used_bytes = g_peak_used > out->used_bytes ? g_peak_used
                                                         : out->used_bytes;
}

void heap_walk_allocated(heap_walk_cb cb, void *ctx)
{
    for (block_hdr_t *b = first_block(); b != NULL; b = next_block(b)) {
        if (!span_ok(b))
            break;
        if (b->magic == HDR_MAGIC_USED)
            cb(payload(b), b->size, b->file, b->line, ctx);
    }
}
