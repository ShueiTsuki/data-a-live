/* SPDX-License-Identifier: MIT */
#include "QcomUsbScreenshot.h"
#include "QcomUsbControl.h"

static uint8_t pixels[QUD_SCREENSHOT_MAX_BYTES];
static unsigned provider_state;
static qcom_usb_screenshot_capture provider;
static void *provider_cookie;
static struct qcom_usb_framebuffer fixed;
static struct qcom_usb_screenshot_info image;
static uint32_t cursor, pixel_cursor, peek_bytes, next_pixel;
static unsigned ready;

static int dimensions(uint32_t w, uint32_t h)
{
    return w && h && w <= QUD_SCREENSHOT_MAX_DIMENSION &&
           h <= QUD_SCREENSHOT_MAX_DIMENSION &&
           (uint64_t)w * h * 3u <= QUD_SCREENSHOT_MAX_BYTES;
}
int qcom_usb_debug_register_screenshot(qcom_usb_screenshot_capture capture, void *cookie)
{
    unsigned expected = 0;
    if (!capture || !__atomic_compare_exchange_n(&provider_state, &expected, 1u,
        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    provider = capture; provider_cookie = cookie;
    __atomic_store_n(&provider_state, 2u, __ATOMIC_RELEASE);
    return 1;
}
static int copy_fixed(void *cookie, uint8_t *rgb, size_t capacity,
                       uint32_t *width, uint32_t *height)
{
    const struct qcom_usb_framebuffer *fb = cookie;
    if (!dimensions(fb->width, fb->height) || (uint64_t)fb->width * fb->height * 3 > capacity) return 0;
    for (uint32_t y = 0; y < fb->height; ++y) {
        const volatile uint8_t *row = fb->pixels + (size_t)y * fb->stride;
        for (uint32_t x = 0; x < fb->width; ++x) {
            size_t at = ((size_t)y * fb->width + x) * 3;
            rgb[at] = row[x * 4 + (fb->format == QCOM_USB_BGRA8888 ? 2 : 0)];
            rgb[at + 1] = row[x * 4 + 1];
            rgb[at + 2] = row[x * 4 + (fb->format == QCOM_USB_BGRA8888 ? 0 : 2)];
        }
    }
    *width = fb->width; *height = fb->height;
    return 1;
}
int qcom_usb_debug_register_framebuffer(const struct qcom_usb_framebuffer *fb)
{
    unsigned expected = 0;
    if (!fb || !fb->pixels || !dimensions(fb->width, fb->height) ||
        (fb->format != QCOM_USB_BGRA8888 && fb->format != QCOM_USB_RGBA8888) ||
        fb->stride < (uint64_t)fb->width * 4 ||
        (uint64_t)fb->stride * fb->height > fb->bytes ||
        fb->bytes > (uintptr_t)-1 - (uintptr_t)fb->pixels ||
        !__atomic_compare_exchange_n(&provider_state, &expected, 1u,
        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    fixed = *fb; provider = copy_fixed; provider_cookie = &fixed;
    __atomic_store_n(&provider_state, 2u, __ATOMIC_RELEASE);
    return 1;
}
static uint32_t run_length(uint32_t at)
{
    uint32_t n = 1, total = image.bytes / 3;
    while (n < 255 && at + n < total &&
           pixels[3 * at] == pixels[3 * (at + n)] &&
           pixels[3 * at + 1] == pixels[3 * (at + n) + 1] &&
           pixels[3 * at + 2] == pixels[3 * (at + n) + 2]) ++n;
    return n;
}
void qcom_usb_shot_cancel(void)
{
    ready = 0; cursor = 0; pixel_cursor = 0; peek_bytes = 0;
}
int qcom_usb_shot_capture(uint32_t frame)
{
    uint32_t w = 0, h = 0, at = 0, encoded = 0;
    qcom_usb_shot_cancel();
    if (__atomic_load_n(&provider_state, __ATOMIC_ACQUIRE) != 2u) return -1;
    if (!provider(provider_cookie, pixels, sizeof(pixels), &w, &h) || !dimensions(w, h)) return -2;
    image.width = w; image.height = h; image.bytes = w * h * 3;
    image.frame = frame; image.crc32 = sxnu_control_crc32(pixels, image.bytes);
    while (at < image.bytes / 3) { at += run_length(at); encoded += 4; }
    image.rle = encoded < image.bytes;
    image.encoded_bytes = image.rle ? encoded : image.bytes;
    ready = 1;
    return 1;
}
int qcom_usb_shot_info(struct qcom_usb_screenshot_info *info)
{
    if (!ready || !info) return 0;
    *info = image;
    return 1;
}
size_t qcom_usb_shot_peek(uint8_t *out, size_t capacity, uint32_t *offset)
{
    size_t n = 0;
    if (!ready || !out || !offset || !capacity) return 0;
    if (capacity > QUD_SCREENSHOT_CHUNK) capacity = QUD_SCREENSHOT_CHUNK;
    *offset = cursor;
    next_pixel = pixel_cursor;
    if (image.rle) {
        while (n + 4 <= capacity && next_pixel < image.bytes / 3) {
            uint32_t run = run_length(next_pixel);
            out[n++] = (uint8_t)run;
            for (unsigned i = 0; i < 3; ++i) out[n++] = pixels[3 * next_pixel + i];
            next_pixel += run;
        }
    } else {
        n = image.encoded_bytes - cursor;
        if (n > capacity) n = capacity;
        for (size_t i = 0; i < n; ++i) out[i] = pixels[cursor + i];
    }
    peek_bytes = (uint32_t)n;
    return n;
}
int qcom_usb_shot_commit(size_t bytes)
{
    if (!ready || !bytes || bytes != peek_bytes || bytes > image.encoded_bytes - cursor) return 0;
    cursor += (uint32_t)bytes; pixel_cursor = next_pixel; peek_bytes = 0;
    return 1;
}
