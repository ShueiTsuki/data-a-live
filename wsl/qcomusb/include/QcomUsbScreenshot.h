/* SPDX-License-Identifier: MIT */
#ifndef QCOM_USB_SCREENSHOT_H
#define QCOM_USB_SCREENSHOT_H
#include <stddef.h>
#include <stdint.h>
#ifndef QUD_SCREENSHOT_MAX_BYTES
#define QUD_SCREENSHOT_MAX_BYTES (16u * 1024u * 1024u)
#endif
#define QUD_SCREENSHOT_CHUNK 192u
#define QUD_SCREENSHOT_MAX_DIMENSION 4096u
struct qcom_usb_screenshot_info {
    uint32_t width, height, bytes, encoded_bytes, crc32, frame;
    uint32_t rle; /* 0: packed RGB888; 1: repeated [count, R, G, B]. */
};
/* Normal USB worker context, outside USB guards, never panic context.
 * Return 1 after copying a complete RGB888 snapshot into caller storage.
 * Must be bounded, CPU coherent, allocation-free, and release all display
 * references/locks before returning. No USB/log calls from this callback.
 */
typedef int (*qcom_usb_screenshot_capture)(void *cookie, uint8_t *rgb,
    size_t capacity, uint32_t *width, uint32_t *height);
enum qcom_usb_framebuffer_format { QCOM_USB_BGRA8888 = 1, QCOM_USB_RGBA8888 = 2 };
struct qcom_usb_framebuffer {
    const volatile uint8_t *pixels; /* Existing CPU mapping, never a PA/IOVA. */
    size_t bytes;
    uint32_t width, height, stride;
    enum qcom_usb_framebuffer_format format;
};
#ifdef __cplusplus
extern "C" {
#endif
/* One provider, registered once. Cookie/mapping stays valid until reset.
 * Use a custom provider for changing scanout buffers, cache sync or locking.
 */
int qcom_usb_debug_register_screenshot(qcom_usb_screenshot_capture capture, void *cookie);
/* Convenience adapter for a permanently mapped, coherent fixed scanout.
 * It does no mapping, cache maintenance or display-service lookup.
 * Concurrent display writes can produce tearing; custom provider may freeze.
 */
int qcom_usb_debug_register_framebuffer(const struct qcom_usb_framebuffer *framebuffer);
#ifdef __cplusplus
}
#endif
#endif
