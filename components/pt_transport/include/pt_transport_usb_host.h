#ifndef PT_TRANSPORT_USB_HOST_H
#define PT_TRANSPORT_USB_HOST_H

/*
 * pt_transport_usb_host - ESP32-S3 native USB Host transport
 * for the Brother QL-700 label printer (VID 0x04F9, PID 0x2042).
 *
 * Uses the ESP-IDF USB Host component. The QL-700's Editor Lite mode
 * (PID 0x2049) is detected separately so the app can explain why the
 * printer interface is unavailable.
 *
 * Target-only; PlatformIO builds this through its ESP-IDF framework.
 */

#include <stdbool.h>
#include <stdint.h>

#include "pt_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pt_transport_usb_host pt_transport_usb_host_t;

/* Install the USB host stack, spawn lib + client tasks, and wait up to
 * connect_timeout_ms for a QL-700 to be enumerated. Returns NULL
 * on any failure (install failed, no matching device attached in time,
 * descriptor parse failed, etc.). */
pt_transport_usb_host_t *pt_transport_usb_host_open(uint32_t connect_timeout_ms);

/* Returns a pt_transport_t bound to *u, valid until close() is called. */
pt_transport_t pt_transport_usb_host_transport(pt_transport_usb_host_t *u);

/* Release the device, free transfers, stop tasks, uninstall stack. */
void pt_transport_usb_host_close(pt_transport_usb_host_t *u);

/* True while the QL-700 is attached in Editor Lite mode (PID 0x2049).
 * The device exposes mass storage instead of a printer interface in that
 * mode; DEV_GONE clears the state when it is unplugged or switched back. */
bool pt_transport_usb_host_editor_lite_seen(void);

/* False once a USB DEV_GONE event has invalidated *u (cable yanked,
 * printer powered off, or switched into Editor Lite mode). The pt_transport_t
 * function pointers themselves stay non-NULL after disconnect, so
 * upper layers can't tell from the transport struct alone whether
 * the device is still alive -- they should use this to gate
 * "transport is ready" checks. */
bool pt_transport_usb_host_alive(const pt_transport_usb_host_t *u);
#ifdef __cplusplus
}
#endif

#endif /* PT_TRANSPORT_USB_HOST_H */
