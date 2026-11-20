/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SC2331_SDIO_H_
#define _SC2331_SDIO_H_

#include <linux/types.h>
#include <linux/device.h>

#define SC2331_SDIO_VENDOR_ID		0x0000
#define SC2331_SDIO_DEVICE_ID		0x2331

#define SC2331_DL_CHN_CALI_WRITE	0
#define SC2331_DL_CHN_WRITE		3
#define SC2331_DL_CHN_READ		12

#define SC2331_WIFI_CHN_TX_0		0
#define SC2331_WIFI_CHN_TX_1		1
#define SC2331_WIFI_CHN_RX_8		8
#define SC2331_WIFI_CHN_RX_9		9

/* Calibration buffer builder (sc2331_cal.c) */
int sc2331_build_cali_buf(struct device *dev, u8 *buf, size_t buf_size);

/* SDIO transport API exposed to Stage-3 WLAN driver */
bool sc2331_sdio_ready(void);
int sc2331_sdio_chn_status(u16 chn_mask, u16 *status);
int sc2331_sdio_get_chn_datalen(u32 chn);
int sc2331_sdio_write(u32 chn, const void *buf, u32 count);
int sc2331_sdio_read(u32 chn, void *buf, u32 *count);
int sc2331_sdio_register_rx_cb(u32 chn, void (*cb)(int chn));
void sc2331_sdio_unregister_rx_cb(u32 chn);
int sc2331_marlin_wakeup(u32 chn, u32 user_id);
int sc2331_marlin_sleep(u32 chn, u32 user_id);
void sc2331_set_wlan_status(int status);
int sc2331_get_rx_gpio_val(void);

/* Stage-3 WLAN bring-up hooks (weak defaults in sc2331_boot.c until wlan is linked) */
int sc2331_wlan_start(struct device *dev);
void sc2331_wlan_stop(struct device *dev);

/* Stage-3 Bluetooth HCI UART (serdev) hooks */
struct serdev_device;
void sc2331_serdev_set_bt_rx(size_t (*cb)(void *data, const u8 *buf, size_t count),
			     void *data);
int sc2331_bt_start(struct serdev_device *serdev);
void sc2331_bt_stop(struct serdev_device *serdev);

#endif /* _SC2331_SDIO_H_ */
