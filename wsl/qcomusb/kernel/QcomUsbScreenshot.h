/* SPDX-License-Identifier: MIT */
#ifndef QCOM_USB_SCREENSHOT_INTERNAL_H
#define QCOM_USB_SCREENSHOT_INTERNAL_H
#include "../include/QcomUsbScreenshot.h"
/* Serialized control-worker-only transaction API. Peek never consumes data;
 * commit only after the complete CRC-framed USB reply is queued. */
int qcom_usb_shot_capture(uint32_t frame);
int qcom_usb_shot_info(struct qcom_usb_screenshot_info *info);
size_t qcom_usb_shot_peek(uint8_t *out, size_t capacity, uint32_t *offset);
int qcom_usb_shot_commit(size_t bytes);
void qcom_usb_shot_cancel(void);
#endif
