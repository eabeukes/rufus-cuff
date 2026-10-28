/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_MEDIATEK_MT6739_CONSYS_H
#define __SOC_MEDIATEK_MT6739_CONSYS_H

#include <linux/types.h>

int mt6739_consys_get(void);
void mt6739_consys_put(void);

/* Bluetooth HCI over STP (H4 framing, type byte first); recv runs in a kthread. */
int mt6739_consys_bt_open(void (*recv)(void *priv, const u8 *data, size_t len), void *priv);
void mt6739_consys_bt_close(void);
int mt6739_consys_bt_send(const u8 *data, size_t len);

#endif
