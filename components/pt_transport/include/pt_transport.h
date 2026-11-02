#ifndef PT_TRANSPORT_H
#define PT_TRANSPORT_H

/*
 * pt_transport - abstract bidirectional byte channel to the QL-700 printer.
 *
 * The QL-700 protocol layer uses this small byte-channel abstraction.
 * The firmware currently provides one implementation: ESP32-S3 USB Host.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Send len bytes. Returns the number of bytes sent (>= 0) or a negative
 * implementation-specific error code. Short writes are not allowed -
 * implementations must either send everything or return an error. */
typedef int (*pt_send_fn)(void *ctx, const uint8_t *data, size_t len);

/* Receive up to cap bytes within timeout_ms. *out_len receives the actual
 * count. Returns 0 on success (including the timeout-with-zero-bytes case)
 * or a negative implementation-specific error. */
typedef int (*pt_recv_fn)(void *ctx, uint8_t *out, size_t cap,
                          size_t *out_len, uint32_t timeout_ms);

typedef struct {
    pt_send_fn  send;
    pt_recv_fn  recv;
    void       *ctx;
} pt_transport_t;

static inline int pt_transport_send(const pt_transport_t *t,
                                    const uint8_t *data, size_t len)
{
    return t->send(t->ctx, data, len);
}

static inline int pt_transport_recv(const pt_transport_t *t,
                                    uint8_t *out, size_t cap,
                                    size_t *out_len, uint32_t timeout_ms)
{
    return t->recv(t->ctx, out, cap, out_len, timeout_ms);
}

#ifdef __cplusplus
}
#endif

#endif /* PT_TRANSPORT_H */
