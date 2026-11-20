// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC2331 (Marlin 1) Bluetooth HCI UART (serdev) driver
 *
 * After Stage-1 (UART0 FDL) and Stage-2 (SDIO main firmware + RF calibration)
 * complete, Marlin 1 exposes a standard Bluetooth H4 HCI transport on UART0
 * at 3,250,000 bps (8N1, hardware RTS/CTS flow control; 48 MHz / 15 = 3.2 Mbps).
 *
 * Initialization sequence verified from stock Samsung SM-J105H libbt-vendor.so:
 *   1. Wake Marlin via GPIO57/59 (sc2331_marlin_wakeup(0xff, 2))
 *   2. Open UART0 @ 3,250,000 bps, 8N1, CRTSCTS
 *   3. Send Spreadtrum vendor PSKEY HCI command:
 *        Opcode = 0xFCA0, plen = 196 (0xC4), payload = pskey[4..199]
 *      populated from sprd/connectivity_configure.ini + sprd/bt_mac.txt
 *   4. BlueZ hci_init0_sync() sends HCI_RESET (0x0C03) immediately after setup()
 */

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/serdev.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci.h>
#include <net/bluetooth/hci_core.h>

#include "sc2331_sdio.h"

#define SC2331_BT_BAUDRATE		3250000
#define SC2331_HCI_OP_PSKEY		0xfca0
#define SC2331_PSKEY_TOTAL_LEN		200
#define SC2331_PSKEY_PAYLOAD_LEN	196
#define SC2331_PSKEY_BDADDR_OFF		0x50
#define SC2331_CONFIG_INI_NAME		"sprd/connectivity_configure.ini"
#define SC2331_BT_MAC_NAME		"sprd/bt_mac.txt"
#define SC2331_WIFI_MAC_NAME		"sprd/wifi_mac.txt"

/* Default 200-byte BT_PSKEY_CONFIG_INFO_T from stock SM-J105H libbt-vendor.so */
static const u8 sc2331_bt_pskey_default[SC2331_PSKEY_TOTAL_LEN] = {
	0x01, 0x01, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xba, 0x8c, 0x01,
	0x00, 0x1f, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0xf6, 0x00, 0x00,
	0x00, 0xd0, 0x00, 0x00, 0x00, 0xaa, 0x00, 0x00, 0x00, 0x84, 0x00, 0x00,
	0x00, 0x44, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0xc8, 0x0f,
	0x00, 0x00, 0xf8, 0x0f, 0x00, 0x00, 0xda, 0x0f, 0x00, 0x00, 0xcc, 0x0f,
	0x00, 0x00, 0xfc, 0x0f, 0xff, 0xff, 0x8d, 0xfe, 0x9b, 0x7f, 0x79, 0x83,
	0xff, 0xa7, 0xff, 0x7f, 0x00, 0xe0, 0xf7, 0x3e, 0x6a, 0x6b, 0x8c, 0x8a,
	0x8b, 0x8c, 0x00, 0x01, 0x05, 0x0e, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x66, 0x00, 0x63, 0x10, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x45, 0x0e, 0x30, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x26, 0x00, 0xff, 0xff, 0x00, 0x00, 0x7c, 0x1a, 0x00, 0x00, 0x7c, 0x19,
	0x00, 0x00, 0x7c, 0x19, 0x00, 0x00, 0x7c, 0x19, 0x00, 0x00, 0x7c, 0x18,
	0x00, 0x00, 0x7c, 0x18, 0x00, 0x00, 0x7c, 0x19, 0x00, 0x00, 0x7c, 0x19,
	0x00, 0x00, 0xfc, 0x17, 0x00, 0x00, 0xfc, 0x15, 0x00, 0x00, 0xfc, 0x16,
	0x00, 0x00, 0xfc, 0x16, 0x00, 0x00, 0xfc, 0x0f, 0x00, 0x00, 0xfc, 0x0f,
	0x00, 0x00, 0x7c, 0x16, 0x00, 0x00, 0x7c, 0x16,
};

struct sc2331_pskey_entry {
	const char *name;
	u16 offset;
	u8 elem_size;
	u8 max_elems;
};

/* Exact 34-key table from stock SM-J105H libbt-vendor.so (.data 0x60e4..0x627b) */
static const struct sc2331_pskey_entry sc2331_bt_pskey_table[] = {
	{ "pskey_cmd",				0x00, 4, 1 },
	{ "g_dbg_source_sink_syn_test_data",	0x04, 1, 1 },
	{ "g_sys_sleep_in_standby_supported",	0x05, 1, 1 },
	{ "g_sys_sleep_master_supported",	0x06, 1, 1 },
	{ "g_sys_sleep_slave_supported",	0x07, 1, 1 },
	{ "default_ahb_clk",			0x08, 4, 1 },
	{ "device_class",			0x0c, 4, 1 },
	{ "win_ext",				0x10, 4, 1 },
	{ "g_aGainValue",			0x14, 4, 6 },
	{ "g_aPowerValue",			0x2c, 4, 5 },
	{ "feature_set",			0x40, 1, 16 },
	{ "device_addr",			0x50, 1, 6 },
	{ "g_sys_sco_transmit_mode",		0x56, 1, 1 },
	{ "g_sys_uart0_communication_supported", 0x57, 1, 1 },
	{ "edr_tx_edr_delay",			0x58, 1, 1 },
	{ "edr_rx_edr_delay",			0x59, 1, 1 },
	{ "g_wbs_nv_117",			0x5a, 2, 1 },
	{ "is_wdg_supported",			0x5c, 4, 1 },
	{ "share_memo_rx_base_addr",		0x60, 4, 1 },
	{ "g_wbs_nv_118",			0x64, 2, 1 },
	{ "g_nbv_nv_117",			0x66, 2, 1 },
	{ "share_memo_tx_packet_num_addr",	0x68, 4, 1 },
	{ "share_memo_tx_data_base_addr",	0x6c, 4, 1 },
	{ "g_PrintLevel",			0x70, 4, 1 },
	{ "share_memo_tx_block_length",		0x74, 2, 1 },
	{ "share_memo_rx_block_length",		0x76, 2, 1 },
	{ "share_memo_tx_water_mark",		0x78, 2, 1 },
	{ "g_nbv_nv_118",			0x7a, 2, 1 },
	{ "uart_rx_watermark",			0x7c, 2, 1 },
	{ "uart_flow_control_thld",		0x7e, 2, 1 },
	{ "comp_id",				0x80, 4, 1 },
	{ "pcm_clk_divd",			0x84, 2, 1 },
	{ "g_aBRChannelpwrvalue",		0x88, 4, 8 },
	{ "g_aEDRChannelpwrvalue",		0xa8, 4, 8 },
};

struct sc2331_bt {
	struct serdev_device *serdev;
	struct device *dev;
	struct hci_dev *hdev;

	struct sk_buff_head txq;
	struct work_struct tx_work;
	bool after_reset;

	spinlock_t rx_lock;
	struct sk_buff *rx_skb;
	u8 rx_pkt_type;
	u16 rx_hdr_len;
	u16 rx_expected_len;
};

static struct sc2331_bt *g_bt;

/* =========================================================================
 * PSKEY INI parser & BT MAC loader
 * ========================================================================= */

static void sc2331_bt_parse_ini(u8 *pskey, const char *data, size_t size)
{
	const char *p = data;
	const char *end = data + size;

	while (p < end) {
		const char *line_start = p;
		const char *line_end, *eq, *kstart, *kend, *v;
		char key[64];
		size_t klen;
		int i;

		while (p < end && *p != '\n' && *p != '\r')
			p++;
		line_end = p;
		while (p < end && (*p == '\n' || *p == '\r'))
			p++;

		/* Trim leading whitespace */
		while (line_start < line_end && isspace((unsigned char)*line_start))
			line_start++;
		if (line_start >= line_end || *line_start == '#' || *line_start == '[')
			continue;

		/* Strip inline '#' comment */
		for (v = line_start; v < line_end; v++) {
			if (*v == '#') {
				line_end = v;
				break;
			}
		}

		eq = memchr(line_start, '=', line_end - line_start);
		if (!eq)
			continue;

		kstart = line_start;
		kend = eq;
		while (kend > kstart && isspace((unsigned char)*(kend - 1)))
			kend--;
		klen = kend - kstart;
		if (!klen || klen >= sizeof(key))
			continue;
		memcpy(key, kstart, klen);
		key[klen] = '\0';

		for (i = 0; i < ARRAY_SIZE(sc2331_bt_pskey_table); i++) {
			const struct sc2331_pskey_entry *e = &sc2331_bt_pskey_table[i];
			int elem = 0;

			if (strcmp(key, e->name) != 0)
				continue;

			v = eq + 1;
			while (v < line_end && elem < e->max_elems) {
				char num[32];
				size_t nlen = 0;
				long val = 0;
				u32 uval;
				size_t off;

				while (v < line_end &&
				       (isspace((unsigned char)*v) || *v == ','))
					v++;
				if (v >= line_end)
					break;
				while (v < line_end && !isspace((unsigned char)*v) &&
				       *v != ',' && nlen < sizeof(num) - 1)
					num[nlen++] = *v++;
				num[nlen] = '\0';
				if (!nlen)
					break;

				if (kstrtol(num, 0, &val)) {
					unsigned long ul = 0;

					if (kstrtoul(num, 0, &ul))
						break;
					uval = (u32)ul;
				} else {
					uval = (u32)val;
				}

				off = e->offset + elem * e->elem_size;
				if (off + e->elem_size > SC2331_PSKEY_TOTAL_LEN)
					break;

				if (e->elem_size == 1)
					pskey[off] = (u8)uval;
				else if (e->elem_size == 2)
					put_unaligned_le16((u16)uval, pskey + off);
				else if (e->elem_size == 4)
					put_unaligned_le32(uval, pskey + off);
				elem++;
			}
			break;
		}
	}
}

static bool sc2331_bt_parse_mac_file(struct device *dev, const char *name, u8 mac[6])
{
	const struct firmware *fw = NULL;
	char buf[32] = {};
	unsigned int b[6];
	int i;

	if (firmware_request_nowarn(&fw, name, dev))
		return false;

	memcpy(buf, fw->data, min_t(size_t, fw->size, sizeof(buf) - 1));
	release_firmware(fw);

	if (sscanf(buf, "%x:%x:%x:%x:%x:%x",
		   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return false;

	for (i = 0; i < 6; i++)
		mac[i] = (u8)b[i];
	return true;
}

static void sc2331_bt_load_bdaddr(struct device *dev, u8 bdaddr[6])
{
	u8 mac[6];

	if (sc2331_bt_parse_mac_file(dev, SC2331_BT_MAC_NAME, mac)) {
		/* Spreadtrum PSKEY device_addr is stored LSB-first (bdaddr_t order) */
		bdaddr[0] = mac[5];
		bdaddr[1] = mac[4];
		bdaddr[2] = mac[3];
		bdaddr[3] = mac[2];
		bdaddr[4] = mac[1];
		bdaddr[5] = mac[0];
		dev_info(dev, "sc2331_bt: loaded factory BT MAC %pM\n", mac);
		return;
	}

	if (sc2331_bt_parse_mac_file(dev, SC2331_WIFI_MAC_NAME, mac)) {
		mac[5] ^= 0x01;
		bdaddr[0] = mac[5];
		bdaddr[1] = mac[4];
		bdaddr[2] = mac[3];
		bdaddr[3] = mac[2];
		bdaddr[4] = mac[1];
		bdaddr[5] = mac[0];
		dev_info(dev, "sc2331_bt: derived BT MAC %pM from Wi-Fi MAC\n", mac);
		return;
	}

	eth_random_addr(mac);
	bdaddr[0] = mac[5];
	bdaddr[1] = mac[4];
	bdaddr[2] = mac[3];
	bdaddr[3] = mac[2];
	bdaddr[4] = mac[1];
	bdaddr[5] = mac[0];
	dev_info(dev, "sc2331_bt: using generated BT MAC %pM\n", mac);
}

/* =========================================================================
 * H4 RX Parser (called from serdev receive_buf)
 * ========================================================================= */

static void sc2331_bt_rx_reset_locked(struct sc2331_bt *bt)
{
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
	bt->rx_pkt_type = 0;
	bt->rx_hdr_len = 0;
	bt->rx_expected_len = 0;
}

static size_t sc2331_bt_rx_cb(void *data, const u8 *buf, size_t count)
{
	struct sc2331_bt *bt = data;
	struct hci_dev *hdev = bt->hdev;
	size_t pos = 0;
	unsigned long flags;

	if (!hdev)
		return count;

	spin_lock_irqsave(&bt->rx_lock, flags);
	while (pos < count) {
		if (!bt->rx_skb) {
			u8 type = buf[pos++];
			u16 hdr_len;

			switch (type) {
			case HCI_EVENT_PKT:
				hdr_len = HCI_EVENT_HDR_SIZE;
				break;
			case HCI_ACLDATA_PKT:
				hdr_len = HCI_ACL_HDR_SIZE;
				break;
			case HCI_SCODATA_PKT:
				hdr_len = HCI_SCO_HDR_SIZE;
				break;
			default:
				/* Resync on unexpected byte */
				continue;
			}

			bt->rx_skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC);
			if (!bt->rx_skb) {
				bt->hdev->stat.err_rx++;
				continue;
			}
			hci_skb_pkt_type(bt->rx_skb) = type;
			bt->rx_pkt_type = type;
			bt->rx_hdr_len = hdr_len;
			bt->rx_expected_len = hdr_len;
			continue;
		}

		/* Fill remaining bytes toward header or payload */
		{
			size_t need = bt->rx_expected_len - bt->rx_skb->len;
			size_t take = min(need, count - pos);

			if (skb_tailroom(bt->rx_skb) < take) {
				sc2331_bt_rx_reset_locked(bt);
				hdev->stat.err_rx++;
				continue;
			}

			skb_put_data(bt->rx_skb, buf + pos, take);
			pos += take;

			/* Once header is complete, compute full packet length */
			if (bt->rx_skb->len == bt->rx_hdr_len &&
			    bt->rx_expected_len == bt->rx_hdr_len) {
				u16 plen = 0;

				if (bt->rx_pkt_type == HCI_EVENT_PKT)
					plen = bt->rx_skb->data[1];
				else if (bt->rx_pkt_type == HCI_ACLDATA_PKT)
					plen = get_unaligned_le16(bt->rx_skb->data + 2);
				else if (bt->rx_pkt_type == HCI_SCODATA_PKT)
					plen = bt->rx_skb->data[2];

				bt->rx_expected_len = bt->rx_hdr_len + plen;
				if (bt->rx_expected_len > HCI_MAX_FRAME_SIZE) {
					sc2331_bt_rx_reset_locked(bt);
					hdev->stat.err_rx++;
					continue;
				}
			}

			/* Deliver complete frame */
			if (bt->rx_skb->len == bt->rx_expected_len) {
				struct sk_buff *complete_skb = bt->rx_skb;

				bt->rx_skb = NULL;
				bt->rx_pkt_type = 0;
				bt->rx_hdr_len = 0;
				bt->rx_expected_len = 0;
				hdev->stat.byte_rx += complete_skb->len + 1;
				spin_unlock_irqrestore(&bt->rx_lock, flags);

				hci_recv_frame(hdev, complete_skb);

				spin_lock_irqsave(&bt->rx_lock, flags);
			}
		}
	}
	spin_unlock_irqrestore(&bt->rx_lock, flags);
	return count;
}

/* =========================================================================
 * H4 TX Worker
 * ========================================================================= */

static void sc2331_bt_tx_work(struct work_struct *work)
{
	struct sc2331_bt *bt = container_of(work, struct sc2331_bt, tx_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&bt->txq)) != NULL) {
		size_t sent = 0;
		bool is_reset;
		int wret;

		if (bt->after_reset) {
			bt->after_reset = false;
			msleep(50);
		}

		is_reset = (skb->len >= 4 &&
			    skb->data[0] == HCI_COMMAND_PKT &&
			    skb->data[1] == 0x03 && skb->data[2] == 0x0c);

		wret = sc2331_marlin_wakeup(0xff, 2);
		if (wret)
			dev_warn_ratelimited(bt->dev, "sc2331_bt: marlin wakeup err %d\n", wret);

		while (sent < skb->len) {
			int n = serdev_device_write(bt->serdev,
						    skb->data + sent,
						    skb->len - sent,
						    msecs_to_jiffies(2000));
			if (n <= 0) {
				dev_err_ratelimited(bt->dev,
						    "sc2331_bt: uart write err %d (sent=%zu/%u)\n",
						    n, sent, skb->len);
				bt->hdev->stat.err_tx++;
				break;
			}
			sent += n;
		}
		if (is_reset)
			bt->after_reset = true;
		bt->hdev->stat.byte_tx += sent;
		kfree_skb(skb);
	}
}

/* =========================================================================
 * hci_dev callbacks
 * ========================================================================= */

static int sc2331_bt_open(struct hci_dev *hdev)
{
	struct sc2331_bt *bt = hci_get_drvdata(hdev);
	unsigned int actual_baud;
	int ret;

	if (!sc2331_sdio_ready())
		return -ENODEV;

	bt->after_reset = false;

	ret = sc2331_marlin_wakeup(0xff, 2);
	if (ret)
		dev_warn(bt->dev, "sc2331_bt: open wakeup err %d\n", ret);

	sc2331_serdev_set_bt_rx(sc2331_bt_rx_cb, bt);

	ret = serdev_device_open(bt->serdev);
	if (ret) {
		sc2331_serdev_set_bt_rx(NULL, NULL);
		dev_err(bt->dev, "sc2331_bt: serdev_device_open failed (%d)\n", ret);
		return ret;
	}

	actual_baud = serdev_device_set_baudrate(bt->serdev, SC2331_BT_BAUDRATE);
	serdev_device_set_flow_control(bt->serdev, true);
	serdev_device_set_parity(bt->serdev, SERDEV_PARITY_NONE);
	dev_dbg(bt->dev, "sc2331_bt: UART0 opened at %u bps (requested %u)\n",
		actual_baud, SC2331_BT_BAUDRATE);
	return 0;
}

static int sc2331_bt_close(struct hci_dev *hdev)
{
	struct sc2331_bt *bt = hci_get_drvdata(hdev);
	unsigned long flags;

	cancel_work_sync(&bt->tx_work);
	skb_queue_purge(&bt->txq);

	sc2331_serdev_set_bt_rx(NULL, NULL);
	serdev_device_close(bt->serdev);

	spin_lock_irqsave(&bt->rx_lock, flags);
	sc2331_bt_rx_reset_locked(bt);
	spin_unlock_irqrestore(&bt->rx_lock, flags);
	return 0;
}

static int sc2331_bt_flush(struct hci_dev *hdev)
{
	struct sc2331_bt *bt = hci_get_drvdata(hdev);

	skb_queue_purge(&bt->txq);
	return 0;
}

static int sc2331_bt_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct sc2331_bt *bt = hci_get_drvdata(hdev);
	u8 pkt_type = hci_skb_pkt_type(skb);

	switch (pkt_type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	default:
		return -EILSEQ;
	}

	memcpy(skb_push(skb, 1), &pkt_type, 1);
	skb_queue_tail(&bt->txq, skb);
	schedule_work(&bt->tx_work);
	return 0;
}

static int sc2331_bt_setup(struct hci_dev *hdev)
{
	struct sc2331_bt *bt = hci_get_drvdata(hdev);
	const struct firmware *fw = NULL;
	struct sk_buff *skb;
	u8 *pskey;
	int ret = 0;

	pskey = kmalloc(SC2331_PSKEY_TOTAL_LEN, GFP_KERNEL);
	if (!pskey)
		return -ENOMEM;

	memcpy(pskey, sc2331_bt_pskey_default, SC2331_PSKEY_TOTAL_LEN);

	if (!firmware_request_nowarn(&fw, SC2331_CONFIG_INI_NAME, bt->dev)) {
		sc2331_bt_parse_ini(pskey, (const char *)fw->data, fw->size);
		release_firmware(fw);
	} else {
		dev_warn(bt->dev, "sc2331_bt: %s not found, using built-in PSKEY defaults\n",
			 SC2331_CONFIG_INI_NAME);
	}

	sc2331_bt_load_bdaddr(bt->dev, pskey + SC2331_PSKEY_BDADDR_OFF);

	/* Ensure Marlin is awake before PSKEY download */
	sc2331_marlin_wakeup(0xff, 2);
	msleep(20);

	/* Send Spreadtrum vendor PSKEY HCI command (0xFCA0, 196 bytes) */
	skb = __hci_cmd_sync(hdev, SC2331_HCI_OP_PSKEY,
			     SC2331_PSKEY_PAYLOAD_LEN, pskey + 4,
			     HCI_INIT_TIMEOUT);
	kfree(pskey);

	if (IS_ERR(skb)) {
		ret = PTR_ERR(skb);
		dev_err(bt->dev, "sc2331_bt: PSKEY cmd 0x%04x failed (%d)\n",
			SC2331_HCI_OP_PSKEY, ret);
		return ret;
	}

	if (skb->len >= 1 && skb->data[0] != 0) {
		dev_err(bt->dev, "sc2331_bt: PSKEY cmd rejected (status=0x%02x)\n",
			skb->data[0]);
		kfree_skb(skb);
		return -EIO;
	}

	if (skb->len >= 7) {
		dev_info(bt->dev, "sc2331_bt: PSKEY OK (fw_ver=0x%04x date=%02x%02x-%02x-%02x)\n",
			 get_unaligned_le16(skb->data + 1),
			 skb->data[6], skb->data[5], skb->data[4], skb->data[3]);
	} else {
		dev_info(bt->dev, "sc2331_bt: PSKEY OK (rsp_len=%u)\n", skb->len);
	}
	kfree_skb(skb);

	/* BlueZ hci_init0_sync() sends HCI_RESET immediately after setup() returns */
	dev_info(bt->dev, "sc2331_bt: Marlin Bluetooth PSKEY loaded!\n");
	return 0;
}

/* =========================================================================
 * Stage-3 Entry / Exit (called from sc2331_boot.c when Marlin is ready)
 * ========================================================================= */

int sc2331_bt_start(struct serdev_device *serdev)
{
	struct sc2331_bt *bt;
	struct hci_dev *hdev;
	int ret;

	if (g_bt || !serdev)
		return 0;

	bt = kzalloc(sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	bt->serdev = serdev;
	bt->dev = &serdev->dev;
	skb_queue_head_init(&bt->txq);
	INIT_WORK(&bt->tx_work, sc2331_bt_tx_work);
	spin_lock_init(&bt->rx_lock);

	hdev = hci_alloc_dev();
	if (!hdev) {
		kfree(bt);
		return -ENOMEM;
	}

	bt->hdev = hdev;
	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, bt);
	hdev->open = sc2331_bt_open;
	hdev->close = sc2331_bt_close;
	hdev->flush = sc2331_bt_flush;
	hdev->setup = sc2331_bt_setup;
	hdev->send = sc2331_bt_send_frame;
	SET_HCIDEV_DEV(hdev, &serdev->dev);

	ret = hci_register_dev(hdev);
	if (ret) {
		dev_err(&serdev->dev, "sc2331_bt: hci_register_dev failed (%d)\n", ret);
		hci_free_dev(hdev);
		kfree(bt);
		return ret;
	}

	g_bt = bt;
	dev_info(&serdev->dev, "sc2331_bt: %s registered on UART0 (%u bps)\n",
		 hdev->name, SC2331_BT_BAUDRATE);
	return 0;
}

void sc2331_bt_stop(struct serdev_device *serdev)
{
	struct sc2331_bt *bt = g_bt;

	if (!bt)
		return;
	g_bt = NULL;

	hci_unregister_dev(bt->hdev);
	hci_free_dev(bt->hdev);
	kfree(bt);
}
