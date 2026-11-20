// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC2331 ("Marlin 1") Combo Chip Boot & SDIO Transport Driver
 *
 * Stage 1: Power on (vddcon 1.6V, vddwifipa 3.3V, clk_aux0 32kHz, reset pulse),
 *          download sprd/sc2331_fdl.bin over UART0 (serdev, 115200 8N1, HDLC),
 *          execute FDL at 0x80000000, and trigger SDIO1 rescan.
 * Stage 2: Bind to SDIO VID 0x0000 PID 0x2331 Func 1, download
 *          sprd/sc2331_fw.bin over SDIO ch 3/12 to 0x00100000, wait for
 *          Marlin ready pulse on wake_ack GPIO, send 4 KiB RF config +
 *          calibration block on SDIO ch 0, then start Stage-3 WLAN.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "../../../../mmc/host/sdhci.h"
#include "sc2331_sdio.h"
#include "sc2331_cal_tables.h"

#define SC2331_FDL_FW_NAME		"sprd/sc2331_fdl.bin"
#define SC2331_MAIN_FW_NAME		"sprd/sc2331_fw.bin"

#define SC2331_FDL_LOAD_ADDR		0x80000000U
#define SC2331_FDL_PADDED_SIZE		0x2000U		/* 8 KiB */
#define SC2331_FDL_CHUNK_SIZE		1024U

#define SC2331_FW_LOAD_ADDR		0x00100000U
#define SC2331_FW_CHUNK_SIZE		(128U * 1024U)	/* 128 KiB */

/* Download protocol commands */
#define BSL_CMD_CONNECT			0x0000
#define BSL_CMD_START_DATA		0x0001
#define BSL_CMD_MIDST_DATA		0x0002
#define BSL_CMD_END_DATA		0x0003
#define BSL_CMD_EXEC_DATA		0x0004
#define BSL_CMD_GET_CHIPID		0x000C
#define BSL_REP_ACK			0x0080

#define HDLC_FLAG			0x7E
#define HDLC_ESC			0x7D
#define HDLC_ESC_MASK			0x20

#define UART_RX_RING_SIZE		512
#define MARLIN_WAKE_HOLD_MS		350
#define MARLIN_ACK_RECHECK_MS		200

struct sc2331_ctx {
	/* Stage 1 (serdev / UART0) */
	struct serdev_device *serdev;
	struct device *boot_dev;
	struct regulator *vdd_con;
	struct regulator *vdd_pa;
	struct clk *lpo_clk;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *wake_out_gpio;
	struct gpio_desc *data_irq_gpio;
	struct gpio_desc *wake_ack_gpio;
	struct device_node *sdio_host_np;

	struct delayed_work boot_work;
	int fw_wait_retries;
	bool powered;

	/* UART0 RX ring & Stage-3 BT RX handoff */
	spinlock_t rx_lock;
	u8 rx_ring[UART_RX_RING_SIZE];
	u16 rx_head;
	u16 rx_tail;
	wait_queue_head_t rx_wq;
	size_t (*bt_rx_cb)(void *data, const u8 *buf, size_t count);
	void *bt_rx_data;

	/* Stage 2 (SDIO) */
	struct sdio_func *func;
	struct mutex sdio_lock;
	struct work_struct sdio_boot_work;
	int data_irq;
	int wake_ack_irq;

	/* Download & Marlin ready state */
	atomic_t download_fin;
	atomic_t ready_seen_high;
	atomic_t ready_done;
	struct completion ready_comp;
	struct completion dl_ack_comp;

	/* Wakeup / sleep handshake */
	struct mutex wake_mutex;
	spinlock_t wake_lock;
	struct completion wake_ack_comp;
	struct timer_list wake_timer;
	bool wake_active;
	bool wake_waiting;
	bool wake_need_pulldown;
	int ack_gpio_last;
	atomic_t wlan_open;

	/* RX callbacks for channels 8..15 */
	void (*rx_cb[16])(int chn);
};

static struct sc2331_ctx *g_ctx;

/* Weak defaults so Milestone 1 compiles standalone before Stage 3 WLAN is linked */
int __weak sc2331_wlan_start(struct device *dev)
{
	dev_info(dev, "sc2331: Stage 3 WLAN not linked yet (Milestone 1 OK)\n");
	return 0;
}

void __weak sc2331_wlan_stop(struct device *dev)
{
}

int __weak sc2331_bt_start(struct serdev_device *serdev)
{
	return 0;
}

void __weak sc2331_bt_stop(struct serdev_device *serdev)
{
}

/* =========================================================================
 * Checksums & HDLC framing (verified from stock download binary)
 * ========================================================================= */

static u16 sc2331_crc16(const u8 *buf, size_t len)
{
	u16 crc = 0;
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		u8 b = buf[i];

		for (bit = 7; bit >= 0; bit--) {
			bool msb = !!(crc & 0x8000);

			crc = (crc << 1) & 0xffff;
			if (msb)
				crc ^= 0x1021;
			if (b & BIT(bit))
				crc ^= 0x1021;
		}
	}
	return crc;
}

/* 16-bit ones-complement checksum used in SDIO download headers (0x3b06) */
static u16 sc2331_csum2(const u8 *buf, size_t len)
{
	u32 sum = 0;
	size_t words = len >> 1;
	size_t i;

	for (i = 0; i < words; i++)
		sum += get_unaligned_le16(buf + (i << 1));
	if (len & 1)
		sum += buf[len - 1];

	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return (u16)(~sum & 0xffff);
}

static size_t hdlc_encode(const u8 *src, size_t src_len, u8 *dst)
{
	size_t i, out = 0;

	dst[out++] = HDLC_FLAG;
	for (i = 0; i < src_len; i++) {
		if (src[i] == HDLC_FLAG) {
			dst[out++] = HDLC_ESC;
			dst[out++] = HDLC_FLAG ^ HDLC_ESC_MASK;
		} else if (src[i] == HDLC_ESC) {
			dst[out++] = HDLC_ESC;
			dst[out++] = HDLC_ESC ^ HDLC_ESC_MASK;
		} else {
			dst[out++] = src[i];
		}
	}
	dst[out++] = HDLC_FLAG;
	return out;
}

static size_t hdlc_decode(const u8 *src, size_t src_len, u8 *dst, size_t dst_max)
{
	size_t i, out = 0;
	bool esc = false;

	for (i = 0; i < src_len; i++) {
		u8 b = src[i];

		if (b == HDLC_FLAG)
			continue;
		if (esc) {
			if (out < dst_max)
				dst[out++] = b ^ HDLC_ESC_MASK;
			esc = false;
		} else if (b == HDLC_ESC) {
			esc = true;
		} else {
			if (out < dst_max)
				dst[out++] = b;
		}
	}
	return out;
}

/* =========================================================================
 * Stage 1: UART0 (serdev) RX ring + HDLC FDL downloader
 * ========================================================================= */

static void uart_rx_flush(struct sc2331_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->rx_lock, flags);
	ctx->rx_head = 0;
	ctx->rx_tail = 0;
	spin_unlock_irqrestore(&ctx->rx_lock, flags);
}

static int uart_rx_getc(struct sc2331_ctx *ctx, u8 *out, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned long flags;

	while (true) {
		spin_lock_irqsave(&ctx->rx_lock, flags);
		if (ctx->rx_head != ctx->rx_tail) {
			*out = ctx->rx_ring[ctx->rx_tail];
			ctx->rx_tail = (ctx->rx_tail + 1) % UART_RX_RING_SIZE;
			spin_unlock_irqrestore(&ctx->rx_lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&ctx->rx_lock, flags);

		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;

		wait_event_timeout(ctx->rx_wq,
				   ctx->rx_head != ctx->rx_tail,
				   max(1UL, deadline - jiffies));
	}
}

void sc2331_serdev_set_bt_rx(size_t (*cb)(void *data, const u8 *buf, size_t count),
			     void *data)
{
	unsigned long flags;

	if (!g_ctx)
		return;
	spin_lock_irqsave(&g_ctx->rx_lock, flags);
	g_ctx->bt_rx_cb = cb;
	g_ctx->bt_rx_data = data;
	spin_unlock_irqrestore(&g_ctx->rx_lock, flags);
}
EXPORT_SYMBOL_GPL(sc2331_serdev_set_bt_rx);

static size_t sc2331_serdev_rx(struct serdev_device *serdev,
			       const u8 *buf, size_t count)
{
	struct sc2331_ctx *ctx = serdev_device_get_drvdata(serdev);
	size_t (*bt_cb)(void *data, const u8 *buf, size_t count);
	void *bt_data;
	unsigned long flags;
	size_t i;

	spin_lock_irqsave(&ctx->rx_lock, flags);
	bt_cb = ctx->bt_rx_cb;
	bt_data = ctx->bt_rx_data;
	if (!bt_cb) {
		for (i = 0; i < count; i++) {
			u16 next = (ctx->rx_head + 1) % UART_RX_RING_SIZE;

			if (next == ctx->rx_tail)
				break;
			ctx->rx_ring[ctx->rx_head] = buf[i];
			ctx->rx_head = next;
		}
	}
	spin_unlock_irqrestore(&ctx->rx_lock, flags);

	if (bt_cb)
		return bt_cb(bt_data, buf, count);

	wake_up_all(&ctx->rx_wq);
	return count;
}

static const struct serdev_device_ops sc2331_serdev_ops = {
	.receive_buf = sc2331_serdev_rx,
	.write_wakeup = serdev_device_write_wakeup,
};

static int uart_write_all(struct serdev_device *serdev, const u8 *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		int n = serdev_device_write(serdev, buf + sent, len - sent,
					    msecs_to_jiffies(2000));
		if (n <= 0)
			return n < 0 ? n : -EIO;
		sent += n;
	}
	serdev_device_wait_until_sent(serdev, msecs_to_jiffies(2000));
	return 0;
}

/* Read one 0x7E ... 0x7E HDLC frame from UART0 */
static int uart_read_hdlc_frame(struct sc2331_ctx *ctx, u8 *raw, size_t max_len,
				unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	size_t pos = 0;
	bool started = false;
	u8 b;

	while (true) {
		unsigned int rem;

		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		rem = jiffies_to_msecs(deadline - jiffies);
		if (rem == 0)
			rem = 1;

		if (uart_rx_getc(ctx, &b, rem))
			return -ETIMEDOUT;

		if (!started) {
			if (b == HDLC_FLAG) {
				started = true;
				pos = 0;
				raw[pos++] = b;
			}
			continue;
		}

		if (b == HDLC_FLAG) {
			if (pos == 1)
				continue; /* leading repeated 0x7E */
			if (pos < max_len)
				raw[pos++] = b;
			return (int)pos;
		}

		if (pos >= max_len - 1) {
			/* garbage / overflow, restart */
			started = false;
			pos = 0;
			continue;
		}
		raw[pos++] = b;
	}
}

static int uart_send_cmd_wait_ack(struct sc2331_ctx *ctx, u16 cmd,
				  const u8 *payload, u16 payload_len,
				  unsigned int timeout_ms)
{
	u8 *plain, *enc;
	u8 raw_rsp[64], dec_rsp[64];
	size_t plain_len = 4 + payload_len + 2;
	size_t enc_len, dec_len;
	u16 crc, rsp_type;
	int ret;

	plain = kmalloc(plain_len, GFP_KERNEL);
	enc = kmalloc(plain_len * 2 + 4, GFP_KERNEL);
	if (!plain || !enc) {
		kfree(plain);
		kfree(enc);
		return -ENOMEM;
	}

	put_unaligned_be16(cmd, plain);
	put_unaligned_be16(payload_len, plain + 2);
	if (payload_len)
		memcpy(plain + 4, payload, payload_len);

	/* CMD_CONNECT uses crc=0x0000 (verified in stock download @ 0x280e) */
	if (cmd == BSL_CMD_CONNECT)
		crc = 0;
	else
		crc = sc2331_crc16(plain, 4 + payload_len);
	put_unaligned_be16(crc, plain + 4 + payload_len);

	enc_len = hdlc_encode(plain, plain_len, enc);
	uart_rx_flush(ctx);
	ret = uart_write_all(ctx->serdev, enc, enc_len);
	kfree(plain);
	kfree(enc);
	if (ret)
		return ret;

	ret = uart_read_hdlc_frame(ctx, raw_rsp, sizeof(raw_rsp), timeout_ms);
	if (ret < 0) {
		dev_err(ctx->boot_dev, "UART cmd 0x%04x ACK timeout (%d)\n",
			cmd, ret);
		return ret;
	}

	dec_len = hdlc_decode(raw_rsp, ret, dec_rsp, sizeof(dec_rsp));
	if (dec_len < 2) {
		dev_err(ctx->boot_dev, "UART cmd 0x%04x short rsp (%zu)\n",
			cmd, dec_len);
		return -EPROTO;
	}

	rsp_type = get_unaligned_be16(dec_rsp);
	if (rsp_type != BSL_REP_ACK) {
		dev_err(ctx->boot_dev, "UART cmd 0x%04x NACK: 0x%04x (len=%zu)\n",
			cmd, rsp_type, dec_len);
		return -EIO;
	}

	return 0;
}

static int sc2331_power_on(struct sc2331_ctx *ctx)
{
	int ret;

	if (!ctx->powered) {
		ret = regulator_enable(ctx->vdd_con);
		if (ret) {
			dev_err(ctx->boot_dev, "enable vdd_con failed (%d)\n", ret);
			return ret;
		}
		if (ctx->vdd_pa) {
			ret = regulator_enable(ctx->vdd_pa);
			if (ret)
				dev_warn(ctx->boot_dev, "enable vdd_pa failed (%d)\n", ret);
		}
		ret = clk_prepare_enable(ctx->lpo_clk);
		if (ret) {
			dev_err(ctx->boot_dev, "enable clk_aux0 failed (%d)\n", ret);
			return ret;
		}
		ctx->powered = true;
		msleep(5);
	}

	/* Reset pulse matching vendor sprd_download_rst(): 1 -> 0 -> 1 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(2);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(2);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(2);

	return 0;
}

static void sc2331_power_off(struct sc2331_ctx *ctx)
{
	if (!ctx->powered)
		return;
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	clk_disable_unprepare(ctx->lpo_clk);
	if (ctx->vdd_pa)
		regulator_disable(ctx->vdd_pa);
	regulator_disable(ctx->vdd_con);
	ctx->powered = false;
}

static int sc2331_uart_handshake(struct sc2331_ctx *ctx)
{
	static const u8 sync_bytes[3] = { HDLC_FLAG, HDLC_FLAG, HDLC_FLAG };
	unsigned long deadline = jiffies + msecs_to_jiffies(3000);
	u8 raw[64], dec[64];
	int ret;

	uart_rx_flush(ctx);
	while (time_before(jiffies, deadline)) {
		serdev_device_write_buf(ctx->serdev, sync_bytes, sizeof(sync_bytes));
		usleep_range(300, 600);
		serdev_device_write_buf(ctx->serdev, sync_bytes, sizeof(sync_bytes));

		ret = uart_read_hdlc_frame(ctx, raw, sizeof(raw), 40);
		if (ret > 2) {
			size_t dec_len = hdlc_decode(raw, ret, dec, sizeof(dec));

			dev_info(ctx->boot_dev,
				 "SC2331 ROM handshake OK (%zu bytes, hdr %02x %02x)\n",
				 dec_len, dec_len > 0 ? dec[0] : 0,
				 dec_len > 1 ? dec[1] : 0);
			return 0;
		}
	}

	dev_err(ctx->boot_dev, "SC2331 UART0 ROM handshake timed out\n");
	return -ETIMEDOUT;
}

static int sc2331_stage1_uart_fdl(struct sc2331_ctx *ctx, const struct firmware *fw)
{
	u8 *fdl_buf;
	u8 payload[8];
	size_t off;
	int ret;

	fdl_buf = kzalloc(SC2331_FDL_PADDED_SIZE, GFP_KERNEL);
	if (!fdl_buf)
		return -ENOMEM;
	memcpy(fdl_buf, fw->data, min_t(size_t, fw->size, SC2331_FDL_PADDED_SIZE));

	ret = serdev_device_open(ctx->serdev);
	if (ret) {
		dev_err(ctx->boot_dev, "serdev_device_open failed (%d)\n", ret);
		kfree(fdl_buf);
		return ret;
	}
	serdev_device_set_baudrate(ctx->serdev, 115200);
	serdev_device_set_flow_control(ctx->serdev, false);
	serdev_device_set_parity(ctx->serdev, SERDEV_PARITY_NONE);

	ret = sc2331_power_on(ctx);
	if (ret)
		goto out_close;

	ret = sc2331_uart_handshake(ctx);
	if (ret)
		goto out_close;

	ret = uart_send_cmd_wait_ack(ctx, BSL_CMD_CONNECT, NULL, 0, 1000);
	if (ret)
		goto out_close;
	dev_info(ctx->boot_dev, "FDL: CMD_CONNECT ACK\n");

	put_unaligned_be32(SC2331_FDL_LOAD_ADDR, payload);
	put_unaligned_be32(SC2331_FDL_PADDED_SIZE, payload + 4);
	ret = uart_send_cmd_wait_ack(ctx, BSL_CMD_START_DATA, payload, 8, 1000);
	if (ret)
		goto out_close;

	for (off = 0; off < SC2331_FDL_PADDED_SIZE; off += SC2331_FDL_CHUNK_SIZE) {
		ret = uart_send_cmd_wait_ack(ctx, BSL_CMD_MIDST_DATA,
					     fdl_buf + off, SC2331_FDL_CHUNK_SIZE,
					     1000);
		if (ret)
			goto out_close;
	}

	ret = uart_send_cmd_wait_ack(ctx, BSL_CMD_END_DATA, NULL, 0, 1000);
	if (ret)
		goto out_close;

	put_unaligned_be32(SC2331_FDL_LOAD_ADDR, payload);
	ret = uart_send_cmd_wait_ack(ctx, BSL_CMD_EXEC_DATA, payload, 4, 1000);
	if (ret)
		goto out_close;

	dev_info(ctx->boot_dev, "FDL loaded & executing at 0x%08x!\n",
		 SC2331_FDL_LOAD_ADDR);

out_close:
	serdev_device_close(ctx->serdev);
	kfree(fdl_buf);
	return ret;
}

static void sc2331_rescan_sdio_host(struct sc2331_ctx *ctx)
{
	struct platform_device *pdev;
	struct sdhci_host *host;

	if (!ctx->sdio_host_np)
		return;

	pdev = of_find_device_by_node(ctx->sdio_host_np);
	if (!pdev) {
		dev_err(ctx->boot_dev, "could not find SDIO host platform device\n");
		return;
	}

	host = platform_get_drvdata(pdev);
	if (host && host->mmc) {
		dev_info(ctx->boot_dev, "triggering SDIO1 rescan on %s\n",
			 mmc_hostname(host->mmc));
		host->mmc->rescan_entered = 0;
		mmc_detect_change(host->mmc, msecs_to_jiffies(50));
	} else {
		dev_err(ctx->boot_dev, "SDIO1 host not probed yet\n");
	}
	put_device(&pdev->dev);
}

static void sc2331_boot_work_fn(struct work_struct *work)
{
	struct sc2331_ctx *ctx = container_of(to_delayed_work(work),
					      struct sc2331_ctx, boot_work);
	const struct firmware *fw = NULL;
	int ret, attempt;

	/* Wait until rootfs is mounted and /lib/firmware/sprd/sc2331_fdl.bin is readable */
	ret = firmware_request_nowarn(&fw, SC2331_FDL_FW_NAME, ctx->boot_dev);
	if (ret) {
		if (++ctx->fw_wait_retries <= 30) {
			schedule_delayed_work(&ctx->boot_work, msecs_to_jiffies(1000));
			return;
		}
		dev_err(ctx->boot_dev, "giving up waiting for %s (%d)\n",
			SC2331_FDL_FW_NAME, ret);
		return;
	}

	dev_info(ctx->boot_dev, "loaded %s (%zu bytes), starting Stage 1 UART0 boot\n",
		 SC2331_FDL_FW_NAME, fw->size);

	for (attempt = 1; attempt <= 5; attempt++) {
		ret = sc2331_stage1_uart_fdl(ctx, fw);
		if (!ret)
			break;
		dev_warn(ctx->boot_dev, "Stage 1 attempt %d/5 failed (%d), power-cycling\n",
			 attempt, ret);
		sc2331_power_off(ctx);
		msleep(100);
	}
	release_firmware(fw);

	if (ret) {
		dev_err(ctx->boot_dev, "Stage 1 FDL boot failed permanently\n");
		return;
	}

	msleep(50);
	sc2331_rescan_sdio_host(ctx);
}

/* =========================================================================
 * Stage 2: SDIO transport + Firmware & Calibration loader
 * ========================================================================= */

bool sc2331_sdio_ready(void)
{
	return g_ctx && atomic_read(&g_ctx->download_fin) == 1 && g_ctx->func;
}
EXPORT_SYMBOL_GPL(sc2331_sdio_ready);

int sc2331_get_rx_gpio_val(void)
{
	if (!g_ctx || !g_ctx->data_irq_gpio)
		return 0;
	return gpiod_get_value(g_ctx->data_irq_gpio);
}
EXPORT_SYMBOL_GPL(sc2331_get_rx_gpio_val);

void sc2331_set_wlan_status(int status)
{
	if (g_ctx)
		atomic_set(&g_ctx->wlan_open, status);
}
EXPORT_SYMBOL_GPL(sc2331_set_wlan_status);

int sc2331_sdio_chn_status(u16 chn_mask, u16 *status)
{
	u8 s0 = 0, s1 = 0;
	int err = 0, ret = 0;

	if (!g_ctx || !g_ctx->func)
		return -ENODEV;

	mutex_lock(&g_ctx->sdio_lock);
	sdio_claim_host(g_ctx->func);

	if (chn_mask & 0x00ff) {
		s0 = sdio_f0_readb(g_ctx->func, 0x840, &err);
		if (err || s0 == 0xff)
			ret = err ? err : -EIO;
	}
	if (!ret && (chn_mask & 0xff00)) {
		s1 = sdio_f0_readb(g_ctx->func, 0x841, &err);
		if (err || s1 == 0xff)
			ret = err ? err : -EIO;
	}

	sdio_release_host(g_ctx->func);
	mutex_unlock(&g_ctx->sdio_lock);

	*status = (((u16)s0 | ((u16)s1 << 8)) & chn_mask);
	return ret;
}
EXPORT_SYMBOL_GPL(sc2331_sdio_chn_status);

int sc2331_sdio_get_chn_datalen(u32 chn)
{
	u8 b0, b1, b2;
	int err0 = 0, err1 = 0, err2 = 0;

	if (!g_ctx || !g_ctx->func)
		return -ENODEV;

	mutex_lock(&g_ctx->sdio_lock);
	sdio_claim_host(g_ctx->func);

	b0 = sdio_f0_readb(g_ctx->func, 0x800 + 4 * chn, &err0);
	b1 = sdio_f0_readb(g_ctx->func, 0x801 + 4 * chn, &err1);
	b2 = sdio_f0_readb(g_ctx->func, 0x802 + 4 * chn, &err2);

	sdio_release_host(g_ctx->func);
	mutex_unlock(&g_ctx->sdio_lock);

	if (err0 || err1 || err2 || (b0 == 0xff && b1 == 0xff && b2 == 0xff))
		return -EIO;

	return (int)(((u32)b0) | ((u32)b1 << 8) | ((u32)b2 << 16));
}
EXPORT_SYMBOL_GPL(sc2331_sdio_get_chn_datalen);

int sc2331_sdio_write(u32 chn, const void *buf, u32 count)
{
	struct sdio_func *func;
	u32 max_chunk, off = 0;
	int ret = 0;

	if (!g_ctx || !g_ctx->func)
		return -ENODEV;
	func = g_ctx->func;

	if (chn != 0 && chn != 1) {
		int avail = sc2331_sdio_get_chn_datalen(chn);

		if (avail < (int)count || avail <= 0) {
			dev_err(&func->dev,
				"sdio_write chn %u avail %d < count %u\n",
				chn, avail, count);
			return -EAGAIN;
		}
	}

	max_chunk = func->card->host->max_req_size;
	if (func->cur_blksize && max_chunk >= func->cur_blksize)
		max_chunk = round_down(max_chunk, func->cur_blksize);
	if (!max_chunk)
		max_chunk = 32768;

	mutex_lock(&g_ctx->sdio_lock);
	sdio_claim_host(func);
	while (off < count) {
		u32 step = min_t(u32, count - off, max_chunk);

		ret = sdio_writesb(func, chn, (u8 *)buf + off, step);
		if (ret)
			break;
		off += step;
	}
	sdio_release_host(func);
	mutex_unlock(&g_ctx->sdio_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(sc2331_sdio_write);

int sc2331_sdio_read(u32 chn, void *buf, u32 *count)
{
	struct sdio_func *func;
	u32 max_chunk, off = 0;
	int len, ret = 0;

	if (!g_ctx || !g_ctx->func)
		return -ENODEV;
	func = g_ctx->func;

	len = sc2331_sdio_get_chn_datalen(chn);
	if (len <= 0)
		return -EAGAIN;

	max_chunk = func->card->host->max_req_size;
	if (func->cur_blksize && max_chunk >= func->cur_blksize)
		max_chunk = round_down(max_chunk, func->cur_blksize);
	if (!max_chunk)
		max_chunk = 32768;

	mutex_lock(&g_ctx->sdio_lock);
	sdio_claim_host(func);
	while (off < (u32)len) {
		u32 step = min_t(u32, (u32)len - off, max_chunk);

		ret = sdio_readsb(func, (u8 *)buf + off, chn, step);
		if (ret)
			break;
		off += step;
	}
	sdio_release_host(func);
	mutex_unlock(&g_ctx->sdio_lock);

	if (!ret && count)
		*count = (u32)len;
	return ret;
}
EXPORT_SYMBOL_GPL(sc2331_sdio_read);

int sc2331_sdio_register_rx_cb(u32 chn, void (*cb)(int chn))
{
	if (!g_ctx || chn >= 16)
		return -EINVAL;
	g_ctx->rx_cb[chn] = cb;
	return 0;
}
EXPORT_SYMBOL_GPL(sc2331_sdio_register_rx_cb);

void sc2331_sdio_unregister_rx_cb(u32 chn)
{
	if (g_ctx && chn < 16)
		g_ctx->rx_cb[chn] = NULL;
}
EXPORT_SYMBOL_GPL(sc2331_sdio_unregister_rx_cb);

/* Marlin wake-out / wake-ack handshake (matches vendor sdio_dev.c) */
static void sc2331_wake_timer_fn(struct timer_list *t)
{
	struct sc2331_ctx *ctx = timer_container_of(ctx, t, wake_timer);
	unsigned long flags;

	spin_lock_irqsave(&ctx->wake_lock, flags);
	if (gpiod_get_value(ctx->wake_ack_gpio)) {
		ctx->wake_active = true;
		mod_timer(&ctx->wake_timer,
			  jiffies + msecs_to_jiffies(MARLIN_ACK_RECHECK_MS));
	} else {
		ctx->wake_active = false;
	}
	spin_unlock_irqrestore(&ctx->wake_lock, flags);
}

int sc2331_marlin_wakeup(u32 chn, u32 user_id)
{
	struct sc2331_ctx *ctx = g_ctx;
	unsigned long flags;
	unsigned long rem;
	int ret = 0;

	if (!ctx || !atomic_read(&ctx->download_fin))
		return -ENODEV;

	mutex_lock(&ctx->wake_mutex);

	spin_lock_irqsave(&ctx->wake_lock, flags);
	if (ctx->wake_active) {
		if (gpiod_get_value(ctx->wake_ack_gpio)) {
			spin_unlock_irqrestore(&ctx->wake_lock, flags);
			mutex_unlock(&ctx->wake_mutex);
			return 0;
		}
		ctx->wake_active = false;
	}

	/* If wake_ack is already high, Marlin is currently awake */
	if (gpiod_get_value(ctx->wake_ack_gpio)) {
		ctx->wake_active = true;
		mod_timer(&ctx->wake_timer,
			  jiffies + msecs_to_jiffies(MARLIN_ACK_RECHECK_MS));
		spin_unlock_irqrestore(&ctx->wake_lock, flags);
		mutex_unlock(&ctx->wake_mutex);
		return 0;
	}

	reinit_completion(&ctx->wake_ack_comp);
	ctx->wake_waiting = true;
	ctx->wake_need_pulldown = true;
	gpiod_set_value(ctx->wake_out_gpio, 1);
	spin_unlock_irqrestore(&ctx->wake_lock, flags);

	rem = wait_for_completion_timeout(&ctx->wake_ack_comp,
					  msecs_to_jiffies(200));
	if (rem == 0) {
		spin_lock_irqsave(&ctx->wake_lock, flags);
		if (gpiod_get_value(ctx->wake_ack_gpio)) {
			ctx->wake_active = true;
			mod_timer(&ctx->wake_timer,
				  jiffies + msecs_to_jiffies(MARLIN_WAKE_HOLD_MS));
			gpiod_set_value(ctx->wake_out_gpio, 0);
			ctx->wake_need_pulldown = false;
			ctx->wake_waiting = false;
			spin_unlock_irqrestore(&ctx->wake_lock, flags);
			mutex_unlock(&ctx->wake_mutex);
			return 0;
		}
		ctx->wake_waiting = false;
		if (ctx->wake_need_pulldown) {
			gpiod_set_value(ctx->wake_out_gpio, 0);
			ctx->wake_need_pulldown = false;
		}
		spin_unlock_irqrestore(&ctx->wake_lock, flags);
		dev_warn_ratelimited(&ctx->func->dev,
				     "marlin wakeup ack timeout (wake_ack=%d)\n",
				     gpiod_get_value(ctx->wake_ack_gpio));
		ret = -ETIMEDOUT;
	}
	mutex_unlock(&ctx->wake_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(sc2331_marlin_wakeup);

int sc2331_marlin_sleep(u32 chn, u32 user_id)
{
	struct sc2331_ctx *ctx = g_ctx;
	unsigned long flags;

	if (!ctx)
		return 0;

	spin_lock_irqsave(&ctx->wake_lock, flags);
	if (ctx->wake_need_pulldown) {
		gpiod_set_value(ctx->wake_out_gpio, 0);
		ctx->wake_need_pulldown = false;
		ctx->wake_waiting = false;
	}
	spin_unlock_irqrestore(&ctx->wake_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(sc2331_marlin_sleep);

static irqreturn_t sc2331_wake_ack_irq_handler(int irq, void *dev_id)
{
	struct sc2331_ctx *ctx = dev_id;
	int val = gpiod_get_value(ctx->wake_ack_gpio);
	unsigned long flags;

	if (!atomic_read(&ctx->download_fin)) {
		if (!atomic_read(&ctx->ready_seen_high) && val) {
			atomic_set(&ctx->ready_seen_high, 1);
		} else if (atomic_read(&ctx->ready_seen_high) && !val &&
			   !atomic_read(&ctx->ready_done)) {
			atomic_set(&ctx->ready_done, 1);
			complete(&ctx->ready_comp);
		}
		return IRQ_HANDLED;
	}

	spin_lock_irqsave(&ctx->wake_lock, flags);
	ctx->ack_gpio_last = val;
	if (!val) {
		ctx->wake_active = false;
	} else if (ctx->wake_need_pulldown) {
		ctx->wake_active = true;
		mod_timer(&ctx->wake_timer,
			  jiffies + msecs_to_jiffies(MARLIN_WAKE_HOLD_MS));
		gpiod_set_value(ctx->wake_out_gpio, 0);
		ctx->wake_need_pulldown = false;
		if (ctx->wake_waiting) {
			ctx->wake_waiting = false;
			complete(&ctx->wake_ack_comp);
		}
	}
	spin_unlock_irqrestore(&ctx->wake_lock, flags);

	return IRQ_HANDLED;
}

static irqreturn_t sc2331_data_irq_handler(int irq, void *dev_id)
{
	struct sc2331_ctx *ctx = dev_id;

	if (!atomic_read(&ctx->download_fin)) {
		complete(&ctx->dl_ack_comp);
	} else if (ctx->rx_cb[SC2331_WIFI_CHN_RX_8]) {
		ctx->rx_cb[SC2331_WIFI_CHN_RX_8](SC2331_WIFI_CHN_RX_8);
	}
	return IRQ_HANDLED;
}

static int sdio_wait_chn3_ready(struct sc2331_ctx *ctx, u32 need_bytes,
				unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	while (time_before(jiffies, deadline)) {
		int avail = sc2331_sdio_get_chn_datalen(SC2331_DL_CHN_WRITE);

		if (avail >= (int)need_bytes)
			return 0;
		usleep_range(1000, 2000);
	}
	return -ETIMEDOUT;
}

static int sdio_wait_dl_rsp(struct sc2331_ctx *ctx, u8 *out, u32 *out_len,
			    unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	while (time_before(jiffies, deadline)) {
		u16 status = 0;

		wait_for_completion_timeout(&ctx->dl_ack_comp, msecs_to_jiffies(10));
		if (!sc2331_sdio_chn_status(BIT(SC2331_DL_CHN_READ), &status) &&
		    (status & BIT(SC2331_DL_CHN_READ))) {
			u32 len = 0;

			if (!sc2331_sdio_read(SC2331_DL_CHN_READ, out, &len) && len > 0) {
				*out_len = len;
				return 0;
			}
		}
	}
	return -ETIMEDOUT;
}

/* Build & send SDIO download packet (flag=1 encoding verified @ 0x27e0) */
static int sdio_send_dl_pkt(struct sc2331_ctx *ctx, u16 cmd,
			    u8 *pkt_buf, u32 data_len,
			    u8 *rsp_buf, u32 *rsp_len, unsigned int timeout_ms)
{
	u32 total_len;
	int ret;

	switch (cmd) {
	case BSL_CMD_GET_CHIPID:
	case BSL_CMD_END_DATA:
		put_unaligned_le16(4, pkt_buf + 0);
		put_unaligned_le16(cmd, pkt_buf + 2);
		put_unaligned_le16(sc2331_csum2(pkt_buf + 8, 0), pkt_buf + 4);
		put_unaligned_le16(sc2331_csum2(pkt_buf, 6), pkt_buf + 6);
		put_unaligned_le32(0, pkt_buf + 8);
		total_len = 12;
		break;
	case BSL_CMD_START_DATA:
	case BSL_CMD_EXEC_DATA:
		put_unaligned_le16(data_len, pkt_buf + 0);
		put_unaligned_le16(cmd, pkt_buf + 2);
		put_unaligned_le16(sc2331_csum2(pkt_buf + 8, data_len), pkt_buf + 4);
		put_unaligned_le16(sc2331_csum2(pkt_buf, 6), pkt_buf + 6);
		total_len = 8 + data_len;
		break;
	case BSL_CMD_MIDST_DATA:
		put_unaligned_le16(data_len & 0xffff, pkt_buf + 0);
		put_unaligned_le16(((data_len & 0xff0000) >> 8) | cmd, pkt_buf + 2);
		put_unaligned_le16(0, pkt_buf + 4);
		put_unaligned_le16(sc2331_csum2(pkt_buf, 6), pkt_buf + 6);
		total_len = 8 + data_len;
		break;
	default:
		return -EINVAL;
	}

	ret = sdio_wait_chn3_ready(ctx, total_len, 2000);
	if (ret) {
		dev_err(&ctx->func->dev, "SDIO ch3 not ready for cmd %u (%u B)\n",
			cmd, total_len);
		return ret;
	}

	reinit_completion(&ctx->dl_ack_comp);
	ret = sc2331_sdio_write(SC2331_DL_CHN_WRITE, pkt_buf, total_len);
	if (ret) {
		dev_err(&ctx->func->dev, "SDIO write cmd %u failed (%d)\n", cmd, ret);
		return ret;
	}

	ret = sdio_wait_dl_rsp(ctx, rsp_buf, rsp_len, timeout_ms);
	if (ret) {
		dev_err(&ctx->func->dev, "SDIO cmd %u ACK timeout\n", cmd);
		return ret;
	}
	return 0;
}

static int sdio_check_ack(struct device *dev, u16 cmd, const u8 *rsp, u32 len)
{
	u16 type;

	if (len < 2) {
		dev_err(dev, "SDIO cmd %u short rsp (%u)\n", cmd, len);
		return -EPROTO;
	}
	type = get_unaligned_be16(rsp);
	if (type != BSL_REP_ACK) {
		dev_err(dev, "SDIO cmd %u NACK: %*ph\n", cmd,
			min_t(u32, len, 8), rsp);
		return -EIO;
	}
	return 0;
}

static void sc2331_sdio_boot_work_fn(struct work_struct *work)
{
	struct sc2331_ctx *ctx = container_of(work, struct sc2331_ctx, sdio_boot_work);
	struct device *dev = &ctx->func->dev;
	const struct firmware *fw = NULL;
	u8 *chunk_buf = NULL, *cali_buf = NULL;
	u8 *cmd_buf = NULL, *rsp_buf = NULL;
	u32 rsp_len = 0, chip_id, count, total_size, i;
	int ret;

	cmd_buf = kzalloc(64, GFP_KERNEL);
	rsp_buf = kzalloc(256, GFP_KERNEL);
	if (!cmd_buf || !rsp_buf)
		goto out_free;

	msleep(20);

	/* 1. Query chip ID (Marlin AA vs BA) */
	ret = sdio_send_dl_pkt(ctx, BSL_CMD_GET_CHIPID, cmd_buf, 0,
			       rsp_buf, &rsp_len, 2000);
	if (ret)
		goto out_free;
	chip_id = rsp_len >= 4 ? get_unaligned_be32(rsp_buf) : 0;
	dev_info(dev, "SDIO chip_id=0x%08x (%s)\n", chip_id,
		 chip_id <= 0x002331fe ? "Marlin AA" :
		 (chip_id >= 0x23311000 && chip_id <= 0x233110fe) ? "Marlin BA" :
		 "other");

	/* 2. Load main firmware sprd/sc2331_fw.bin */
	ret = request_firmware(&fw, SC2331_MAIN_FW_NAME, dev);
	if (ret) {
		dev_err(dev, "failed to load %s (%d)\n", SC2331_MAIN_FW_NAME, ret);
		goto out_free;
	}

	count = DIV_ROUND_UP(fw->size, SC2331_FW_CHUNK_SIZE);
	total_size = count * SC2331_FW_CHUNK_SIZE;
	dev_info(dev, "downloading %s (%zu B -> %u chunks, %u B @ 0x%08x)\n",
		 SC2331_MAIN_FW_NAME, fw->size, count, total_size, SC2331_FW_LOAD_ADDR);

	chunk_buf = alloc_pages_exact(8 + SC2331_FW_CHUNK_SIZE, GFP_KERNEL);
	if (!chunk_buf)
		goto out_free;

	/* 3. DATA_START */
	memset(cmd_buf, 0, 64);
	put_unaligned_be32(SC2331_FW_LOAD_ADDR, cmd_buf + 8);
	put_unaligned_be32(total_size, cmd_buf + 12);
	ret = sdio_send_dl_pkt(ctx, BSL_CMD_START_DATA, cmd_buf, 8,
			       rsp_buf, &rsp_len, 2000);
	if (ret || sdio_check_ack(dev, BSL_CMD_START_DATA, rsp_buf, rsp_len))
		goto out_free;

	/* 4. DATA_MIDST chunks (128 KiB each, tail padded with 0xFF) */
	for (i = 0; i < count; i++) {
		size_t off = (size_t)i * SC2331_FW_CHUNK_SIZE;
		size_t copy_len = min_t(size_t, SC2331_FW_CHUNK_SIZE, fw->size - off);

		memcpy(chunk_buf + 8, fw->data + off, copy_len);
		if (copy_len < SC2331_FW_CHUNK_SIZE)
			memset(chunk_buf + 8 + copy_len, 0xff,
			       SC2331_FW_CHUNK_SIZE - copy_len);

		ret = sdio_send_dl_pkt(ctx, BSL_CMD_MIDST_DATA, chunk_buf,
				       SC2331_FW_CHUNK_SIZE, rsp_buf, &rsp_len, 2000);
		if (ret || sdio_check_ack(dev, BSL_CMD_MIDST_DATA, rsp_buf, rsp_len))
			goto out_free;
	}

	/* 5. DATA_END */
	memset(cmd_buf, 0, 64);
	ret = sdio_send_dl_pkt(ctx, BSL_CMD_END_DATA, cmd_buf, 0,
			       rsp_buf, &rsp_len, 2000);
	if (ret || sdio_check_ack(dev, BSL_CMD_END_DATA, rsp_buf, rsp_len))
		goto out_free;

	/* 6. CMD_EXEC */
	reinit_completion(&ctx->ready_comp);
	atomic_set(&ctx->ready_seen_high, 0);
	atomic_set(&ctx->ready_done, 0);

	memset(cmd_buf, 0, 64);
	put_unaligned_be32(SC2331_FW_LOAD_ADDR, cmd_buf + 8);
	ret = sdio_send_dl_pkt(ctx, BSL_CMD_EXEC_DATA, cmd_buf, 4,
			       rsp_buf, &rsp_len, 2000);
	if (ret || sdio_check_ack(dev, BSL_CMD_EXEC_DATA, rsp_buf, rsp_len))
		goto out_free;

	dev_info(dev, "main firmware executing, waiting for Marlin ready pulse...\n");

	/* 7. Wait for Marlin ready pulse (wake_ack HIGH -> LOW) */
	if (!wait_for_completion_timeout(&ctx->ready_comp, msecs_to_jiffies(5000))) {
		dev_warn(dev, "Marlin ready pulse timeout (seen_high=%d, wake_ack=%d), proceeding\n",
			 atomic_read(&ctx->ready_seen_high),
			 gpiod_get_value_cansleep(ctx->wake_ack_gpio));
	} else {
		dev_info(dev, "Marlin ready pulse received!\n");
	}

	/* 8. Send 4096-byte RF config + calibration struct on SDIO ch 0 */
	cali_buf = kzalloc(SC2331_CALI_BUF_SIZE, GFP_KERNEL);
	if (!cali_buf)
		goto out_free;

	ret = sc2331_build_cali_buf(dev, cali_buf, SC2331_CALI_BUF_SIZE);
	if (ret)
		goto out_free;

	ret = sc2331_sdio_write(SC2331_DL_CHN_CALI_WRITE, cali_buf,
				SC2331_CALI_BUF_SIZE);
	if (ret) {
		dev_err(dev, "failed to write calibration block (%d)\n", ret);
		goto out_free;
	}

	/* If not pre-calibrated, wait briefly for firmware calibration response */
	if (cali_buf[SC2331_CALI_DATA_OFFSET + 1] == 0) {
		reinit_completion(&ctx->dl_ack_comp);
		sdio_wait_dl_rsp(ctx, rsp_buf, &rsp_len, 2000);
	}

	atomic_set(&ctx->download_fin, 1);
	dev_info(dev, "sc2331: firmware & calibration loaded, Marlin ready!\n");

	/* 9. Launch Stage-3 WLAN and Bluetooth drivers */
	ret = sc2331_wlan_start(dev);
	if (ret)
		dev_err(dev, "sc2331_wlan_start failed (%d)\n", ret);

	/* Give Marlin CP2 time to finish calibration processing before BT HCI */
	msleep(500);
	ret = sc2331_bt_start(ctx->serdev);
	if (ret)
		dev_err(dev, "sc2331_bt_start failed (%d)\n", ret);

out_free:
	kfree(cali_buf);
	kfree(cmd_buf);
	kfree(rsp_buf);
	if (chunk_buf)
		free_pages_exact(chunk_buf, 8 + SC2331_FW_CHUNK_SIZE);
	if (fw)
		release_firmware(fw);
}

static int sc2331_sdio_probe(struct sdio_func *func,
			     const struct sdio_device_id *id)
{
	struct sc2331_ctx *ctx = g_ctx;
	int ret;

	if (!ctx) {
		dev_err(&func->dev, "SC2331 boot context not initialized\n");
		return -ENODEV;
	}
	if (func->num != 1)
		return 0;

	dev_info(&func->dev, "SC2331 SDIO Func %u matched (VID=0x%04x PID=0x%04x, max_req=%u, max_blk=%u)\n",
		 func->num, func->vendor, func->device,
		 func->card->host->max_req_size, func->card->host->max_blk_count);

	ctx->func = func;
	sdio_set_drvdata(func, ctx);

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	if (!ret)
		ret = sdio_set_block_size(func, 512);
	sdio_release_host(func);
	if (ret) {
		dev_err(&func->dev, "enable func1 / set_block_size failed (%d)\n", ret);
		ctx->func = NULL;
		return ret;
	}

	ctx->wake_ack_irq = gpiod_to_irq(ctx->wake_ack_gpio);
	if (ctx->wake_ack_irq >= 0) {
		ret = request_irq(ctx->wake_ack_irq, sc2331_wake_ack_irq_handler,
				  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_NO_SUSPEND,
				  "sc2331_wake_ack", ctx);
		if (ret) {
			dev_err(&func->dev, "request wake_ack irq failed (%d)\n", ret);
			ctx->wake_ack_irq = -1;
		}
	}

	ctx->data_irq = gpiod_to_irq(ctx->data_irq_gpio);
	if (ctx->data_irq >= 0) {
		ret = request_irq(ctx->data_irq, sc2331_data_irq_handler,
				  IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
				  "sc2331_data_irq", ctx);
		if (ret) {
			dev_err(&func->dev, "request data_irq failed (%d)\n", ret);
			ctx->data_irq = -1;
		}
	}

	schedule_work(&ctx->sdio_boot_work);
	return 0;
}

static void sc2331_sdio_remove(struct sdio_func *func)
{
	struct sc2331_ctx *ctx = sdio_get_drvdata(func);

	if (!ctx || func->num != 1)
		return;

	if (atomic_read(&ctx->download_fin)) {
		sc2331_bt_stop(ctx->serdev);
		sc2331_wlan_stop(&func->dev);
	}

	atomic_set(&ctx->download_fin, 0);
	cancel_work_sync(&ctx->sdio_boot_work);
	timer_delete_sync(&ctx->wake_timer);

	if (ctx->data_irq >= 0) {
		free_irq(ctx->data_irq, ctx);
		ctx->data_irq = -1;
	}
	if (ctx->wake_ack_irq >= 0) {
		free_irq(ctx->wake_ack_irq, ctx);
		ctx->wake_ack_irq = -1;
	}

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	ctx->func = NULL;
}

static const struct sdio_device_id sc2331_sdio_ids[] = {
	{ SDIO_DEVICE(SC2331_SDIO_VENDOR_ID, SC2331_SDIO_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, sc2331_sdio_ids);

static struct sdio_driver sc2331_sdio_driver = {
	.name		= "sc2331_sdio",
	.id_table	= sc2331_sdio_ids,
	.probe		= sc2331_sdio_probe,
	.remove		= sc2331_sdio_remove,
};

/* =========================================================================
 * Stage 1 serdev platform probe
 * ========================================================================= */

static int sc2331_serdev_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct sc2331_ctx *ctx;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->serdev = serdev;
	ctx->boot_dev = dev;
	ctx->data_irq = -1;
	ctx->wake_ack_irq = -1;
	spin_lock_init(&ctx->rx_lock);
	spin_lock_init(&ctx->wake_lock);
	mutex_init(&ctx->wake_mutex);
	init_waitqueue_head(&ctx->rx_wq);
	mutex_init(&ctx->sdio_lock);
	init_completion(&ctx->ready_comp);
	init_completion(&ctx->dl_ack_comp);
	init_completion(&ctx->wake_ack_comp);
	timer_setup(&ctx->wake_timer, sc2331_wake_timer_fn, 0);
	INIT_DELAYED_WORK(&ctx->boot_work, sc2331_boot_work_fn);
	INIT_WORK(&ctx->sdio_boot_work, sc2331_sdio_boot_work_fn);

	ctx->vdd_con = devm_regulator_get(dev, "vdd-con");
	if (IS_ERR(ctx->vdd_con))
		return dev_err_probe(dev, PTR_ERR(ctx->vdd_con), "missing vdd-con\n");

	ctx->vdd_pa = devm_regulator_get_optional(dev, "vdd-pa");
	if (IS_ERR(ctx->vdd_pa)) {
		if (PTR_ERR(ctx->vdd_pa) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		ctx->vdd_pa = NULL;
	}

	ctx->lpo_clk = devm_clk_get(dev, "lpo");
	if (IS_ERR(ctx->lpo_clk))
		return dev_err_probe(dev, PTR_ERR(ctx->lpo_clk), "missing lpo clk\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "missing reset-gpios\n");

	ctx->wake_out_gpio = devm_gpiod_get(dev, "wake-out", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->wake_out_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->wake_out_gpio), "missing wake-out-gpios\n");

	ctx->data_irq_gpio = devm_gpiod_get(dev, "data-irq", GPIOD_IN);
	if (IS_ERR(ctx->data_irq_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->data_irq_gpio), "missing data-irq-gpios\n");

	ctx->wake_ack_gpio = devm_gpiod_get(dev, "wake-ack", GPIOD_IN);
	if (IS_ERR(ctx->wake_ack_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->wake_ack_gpio), "missing wake-ack-gpios\n");

	ctx->sdio_host_np = of_parse_phandle(dev->of_node, "sprd,sdio-host", 0);

	serdev_device_set_drvdata(serdev, ctx);
	serdev_device_set_client_ops(serdev, &sc2331_serdev_ops);
	g_ctx = ctx;

	dev_info(dev, "SC2331 combo controller probed, scheduling firmware boot\n");
	schedule_delayed_work(&ctx->boot_work, msecs_to_jiffies(2000));
	return 0;
}

static void sc2331_serdev_remove(struct serdev_device *serdev)
{
	struct sc2331_ctx *ctx = serdev_device_get_drvdata(serdev);

	cancel_delayed_work_sync(&ctx->boot_work);
	sc2331_power_off(ctx);
	of_node_put(ctx->sdio_host_np);
	g_ctx = NULL;
}

static const struct of_device_id sc2331_of_match[] = {
	{ .compatible = "sprd,sc2331" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2331_of_match);

static struct serdev_device_driver sc2331_serdev_driver = {
	.probe = sc2331_serdev_probe,
	.remove = sc2331_serdev_remove,
	.driver = {
		.name = "sprd-sc2331",
		.of_match_table = sc2331_of_match,
	},
};

static int __init sc2331_init(void)
{
	int ret;

	ret = sdio_register_driver(&sc2331_sdio_driver);
	if (ret)
		return ret;

	ret = serdev_device_driver_register(&sc2331_serdev_driver);
	if (ret) {
		sdio_unregister_driver(&sc2331_sdio_driver);
		return ret;
	}
	return 0;
}

static void __exit sc2331_exit(void)
{
	serdev_device_driver_unregister(&sc2331_serdev_driver);
	sdio_unregister_driver(&sc2331_sdio_driver);
}

module_init(sc2331_init);
module_exit(sc2331_exit);

MODULE_DESCRIPTION("Spreadtrum SC2331 (Marlin 1) Wi-Fi/BT boot and SDIO driver");
MODULE_LICENSE("GPL");
