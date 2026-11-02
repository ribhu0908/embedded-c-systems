/*
 * ringbuf.h - Lock-free single-producer / single-consumer byte ring buffer.
 *
 * Typical use: ISR (producer) pushes UART/DMA bytes, a task (consumer) drains
 * them - no mutex, no disabling interrupts. Capacity must be a power of two so
 * wrap-around is a mask instead of a modulo, and head/tail run free so "full"
 * and "empty" never need a wasted slot.
 */
#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t          *buf;
    uint32_t          mask;   /* capacity - 1                      */
    _Atomic uint32_t  head;   /* written only by the producer      */
    _Atomic uint32_t  tail;   /* written only by the consumer      */
} ringbuf_t;

/* Returns 0 on success, -1 if capacity is not a non-zero power of two. */
int    rb_init(ringbuf_t *rb, uint8_t *storage, uint32_t capacity);

size_t rb_used(const ringbuf_t *rb);
size_t rb_free(const ringbuf_t *rb);

/* Producer side: copies up to len bytes, returns bytes actually written. */
size_t rb_write(ringbuf_t *rb, const void *data, size_t len);

/* Consumer side: copies up to len bytes, returns bytes actually read. */
size_t rb_read(ringbuf_t *rb, void *out, size_t len);

/* Consumer side: look without consuming. */
size_t rb_peek(const ringbuf_t *rb, void *out, size_t len);

#endif /* RINGBUF_H */
