/*
 * ringbuf.c - see include/ringbuf.h
 *
 * Memory ordering:
 *   producer: copy bytes -> store head with RELEASE
 *   consumer: load head with ACQUIRE -> copy bytes -> store tail with RELEASE
 * The acquire/release pair guarantees the consumer never sees an advanced
 * head before the bytes behind it are visible (matters on multi-core ARM,
 * harmless on a single core).
 */
#include "ringbuf.h"

#include <string.h>

int rb_init(ringbuf_t *rb, uint8_t *storage, uint32_t capacity)
{
    if (rb == NULL || storage == NULL || capacity == 0 ||
        (capacity & (capacity - 1)) != 0)
        return -1;

    rb->buf  = storage;
    rb->mask = capacity - 1;
    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
    return 0;
}

size_t rb_used(const ringbuf_t *rb)
{
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (size_t)(head - tail);       /* unsigned wrap makes this correct */
}

size_t rb_free(const ringbuf_t *rb)
{
    return (size_t)rb->mask + 1 - rb_used(rb);
}

/* Copy len bytes into the ring starting at logical index idx (handles wrap). */
static void copy_in(ringbuf_t *rb, uint32_t idx, const uint8_t *src, size_t len)
{
    uint32_t off   = idx & rb->mask;
    size_t   first = (size_t)rb->mask + 1 - off;
    if (first > len)
        first = len;
    memcpy(rb->buf + off, src, first);
    memcpy(rb->buf, src + first, len - first);
}

static void copy_out(const ringbuf_t *rb, uint32_t idx, uint8_t *dst, size_t len)
{
    uint32_t off   = idx & rb->mask;
    size_t   first = (size_t)rb->mask + 1 - off;
    if (first > len)
        first = len;
    memcpy(dst, rb->buf + off, first);
    memcpy(dst + first, rb->buf, len - first);
}

size_t rb_write(ringbuf_t *rb, const void *data, size_t len)
{
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    size_t   space = (size_t)rb->mask + 1 - (size_t)(head - tail);

    if (len > space)
        len = space;
    if (len == 0)
        return 0;

    copy_in(rb, head, (const uint8_t *)data, len);
    atomic_store_explicit(&rb->head, head + (uint32_t)len, memory_order_release);
    return len;
}

size_t rb_peek(const ringbuf_t *rb, void *out, size_t len)
{
    uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t   avail = (size_t)(head - tail);

    if (len > avail)
        len = avail;
    if (len)
        copy_out(rb, tail, (uint8_t *)out, len);
    return len;
}

size_t rb_read(ringbuf_t *rb, void *out, size_t len)
{
    len = rb_peek(rb, out, len);
    if (len) {
        uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
        atomic_store_explicit(&rb->tail, tail + (uint32_t)len,
                              memory_order_release);
    }
    return len;
}
