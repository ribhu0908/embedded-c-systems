/*
 * dbg_heap.h - Fixed-arena first-fit heap with corruption detection.
 *
 * Built for firmware where there is no MMU, no ASan and a crash dump is all
 * you get. Every block records WHO allocated it (file:line), carries a header
 * magic and a trailing canary, and the whole arena can be walked on demand to
 * find the first corrupted block and its owner.
 */
#ifndef DBG_HEAP_H
#define DBG_HEAP_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    HEAP_OK = 0,
    HEAP_ERR_HEADER_MAGIC,   /* header overwritten (underflow of prev block) */
    HEAP_ERR_TAIL_CANARY,    /* write past end of user region (overflow)     */
    HEAP_ERR_BAD_SIZE,       /* block size field is corrupt                   */
    HEAP_ERR_DOUBLE_FREE,
    HEAP_ERR_INVALID_PTR     /* pointer not owned by this heap                */
} heap_status_t;

typedef struct {
    size_t total_bytes;      /* arena size                                    */
    size_t used_bytes;       /* user bytes currently allocated                */
    size_t free_bytes;       /* bytes in free blocks (payload only)           */
    size_t largest_free;     /* biggest single allocation possible right now  */
    size_t peak_used_bytes;  /* high-water mark                               */
    size_t alloc_count;      /* live allocations                              */
    size_t free_blocks;      /* count of free blocks (fragmentation signal)   */
} heap_stats_t;

typedef struct {
    heap_status_t status;
    size_t        block_offset; /* offset of bad block from arena start       */
    const char   *owner_file;   /* allocator of the bad block (if readable)   */
    int           owner_line;
} heap_report_t;

/* Initialise heap over caller-provided memory (e.g. a linker section). */
int  heap_init(void *arena, size_t size);

/* Use the macros, not the _dbg functions, so the call site is captured. */
#define HEAP_MALLOC(sz) heap_malloc_dbg((sz), __FILE__, __LINE__)
#define HEAP_FREE(p)    heap_free_dbg((p), __FILE__, __LINE__)

void         *heap_malloc_dbg(size_t size, const char *file, int line);
heap_status_t heap_free_dbg(void *ptr, const char *file, int line);

/* Walk every block and validate magic, canary and size chain. */
heap_report_t heap_check(void);

void heap_get_stats(heap_stats_t *out);

/* Calls cb for each live allocation - use it to dump leaks at shutdown. */
typedef void (*heap_walk_cb)(const void *ptr, size_t size,
                             const char *file, int line, void *ctx);
void heap_walk_allocated(heap_walk_cb cb, void *ctx);

#endif /* DBG_HEAP_H */
