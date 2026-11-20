// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC2331 ("Marlin 1") FullMAC cfg80211 Wi-Fi driver for Linux 7.3+
 *
 * Implements the CP2 SDIO command/event/data framing from the SC2331 firmware
 * over sc2331_boot's SDIO transport layer.
 */

#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/gfp.h>
#include <linux/ieee80211.h>
#include <linux/inetdevice.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/semaphore.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <net/cfg80211.h>

#include "sc2331_sdio.h"
#include "sc2331_wlan.h"

/* =========================================================================
 * Driver state structures
 * ========================================================================= */

struct sc2331_tx_msg {
	struct sc2331_msg_hdr hdr;
	void *data;
	u16 data_len;
	struct sk_buff *skb;
};

struct sc2331_msg_q {
	struct sc2331_tx_msg *ring;
	u32 num;
	u32 wt;
	u32 rd;
	u32 wt_cnt;
	u32 rd_cnt;
	spinlock_t lock;
};

struct sc2331_tx_fifo {
	u8 *raw_mem;
	u32 raw_size;
	u8 *mem;
	u32 size;
	u32 cp2_tx_ram;
	u16 max_msg_num;
	u32 wt;
	u32 rd;
	int last_wt;
	u32 seq;
	spinlock_t lock;
};

struct sc2331_rx_fifo {
	u32 wt;
	u32 rd;
	u8 *blocks[SC2331_RX_FIFO_BLOCKS];
	u32 lens[SC2331_RX_FIFO_BLOCKS];
};

struct sc2331_vif {
	struct net_device *ndev;
	struct wireless_dev wdev;
	u8 id;
	int mode;
	spinlock_t state_lock;
	enum sc2331_conn_state conn_state;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	int ssid_len;
	u8 bssid[ETH_ALEN];

	/* Scan state */
	struct cfg80211_scan_request *scan_req;
	atomic_t scan_lock;
	struct timer_list scan_timer;
	struct sc2331_scan_entry *scan_bufs;

	/* Queues */
	struct sc2331_msg_q cmd_q;
	struct sc2331_msg_q data_q;
	struct sc2331_tx_fifo tx_fifo;
};

struct sc2331_wlan {
	struct device *dev;
	struct wiphy *wiphy;
	struct sc2331_vif vif;
	struct sc2331_rx_fifo rx_fifo;

	/* Command RPC */
	struct mutex cmd_lock;
	struct mutex rsp_lock;
	wait_queue_head_t rsp_wq;
	int rsp_ready;
	u8 *rsp_buf;

	/* Threads & sync */
	struct task_struct *trans_task;
	struct task_struct *core_task;
	struct semaphore trans_sem;
	struct semaphore core_sem;
	atomic_t exit_flag;

	/* SDIO counters & channels */
	u32 tx_cnt;
	u32 rx_cnt;
	struct notifier_block inetaddr_nb;
};

static struct sc2331_wlan *g_wlan;

static inline struct sc2331_vif *ndev_to_sc2331_vif(struct net_device *ndev)
{
	return *(struct sc2331_vif **)netdev_priv(ndev);
}

/* =========================================================================
 * 2.4 GHz band & rate definitions
 * ========================================================================= */

#define SC_RATE(_rate, _id) { .bitrate = (_rate), .hw_value = (_id) }
#define SC_CHAN2G(_ch, _freq) { \
	.band = NL80211_BAND_2GHZ, \
	.center_freq = (_freq), \
	.hw_value = (_ch), \
	.max_power = 30, \
}

static struct ieee80211_rate sc2331_rates[] = {
	SC_RATE(10, 0x01), SC_RATE(20, 0x02), SC_RATE(55, 0x05), SC_RATE(110, 0x0b),
	SC_RATE(60, 0x06), SC_RATE(90, 0x09), SC_RATE(120, 0x0c), SC_RATE(180, 0x12),
	SC_RATE(240, 0x18), SC_RATE(360, 0x24), SC_RATE(480, 0x30), SC_RATE(540, 0x36),
	SC_RATE(65, 0x80), SC_RATE(130, 0x81), SC_RATE(195, 0x82), SC_RATE(260, 0x83),
	SC_RATE(390, 0x84), SC_RATE(520, 0x85), SC_RATE(585, 0x86), SC_RATE(650, 0x87),
};

static struct ieee80211_channel sc2331_2ghz_channels[] = {
	SC_CHAN2G(1, 2412), SC_CHAN2G(2, 2417), SC_CHAN2G(3, 2422),
	SC_CHAN2G(4, 2427), SC_CHAN2G(5, 2432), SC_CHAN2G(6, 2437),
	SC_CHAN2G(7, 2442), SC_CHAN2G(8, 2447), SC_CHAN2G(9, 2452),
	SC_CHAN2G(10, 2457), SC_CHAN2G(11, 2462), SC_CHAN2G(12, 2467),
	SC_CHAN2G(13, 2472), SC_CHAN2G(14, 2484),
};

static struct ieee80211_supported_band sc2331_band_2ghz = {
	.n_channels = ARRAY_SIZE(sc2331_2ghz_channels),
	.channels = sc2331_2ghz_channels,
	.n_bitrates = ARRAY_SIZE(sc2331_rates),
	.bitrates = sc2331_rates,
	.ht_cap = {
		.ht_supported = true,
		.cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
		       IEEE80211_HT_CAP_SGI_20 |
		       IEEE80211_HT_CAP_SGI_40,
		.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
		.ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,
		.mcs = {
			.rx_mask = { 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		},
	},
};

static const u32 sc2331_ciphers[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
};

/* =========================================================================
 * Simple ring queues & CP2 TX/RX FIFOs
 * ========================================================================= */

static int msg_q_init(struct sc2331_msg_q *q, u32 num)
{
	q->ring = kcalloc(num, sizeof(*q->ring), GFP_KERNEL);
	if (!q->ring)
		return -ENOMEM;
	q->num = num;
	q->wt = q->rd = q->wt_cnt = q->rd_cnt = 0;
	spin_lock_init(&q->lock);
	return 0;
}

static void msg_q_destroy(struct sc2331_msg_q *q)
{
	while (q->ring && q->rd != q->wt) {
		struct sc2331_tx_msg *m = &q->ring[q->rd];

		if (m->skb)
			dev_kfree_skb_any(m->skb);
		else
			kfree(m->data);
		q->rd = (q->rd + 1) % q->num;
	}
	kfree(q->ring);
	q->ring = NULL;
}

static int msg_q_push(struct sc2331_msg_q *q, const struct sc2331_tx_msg *m)
{
	unsigned long flags;

	spin_lock_irqsave(&q->lock, flags);
	if (((q->wt + 1) % q->num) == q->rd) {
		spin_unlock_irqrestore(&q->lock, flags);
		return -ENOSPC;
	}
	q->ring[q->wt] = *m;
	q->wt = (q->wt + 1) % q->num;
	q->wt_cnt++;
	spin_unlock_irqrestore(&q->lock, flags);
	return 0;
}

static struct sc2331_tx_msg *msg_q_peek(struct sc2331_msg_q *q)
{
	if (q->wt == q->rd)
		return NULL;
	return &q->ring[q->rd];
}

static void msg_q_pop(struct sc2331_msg_q *q)
{
	if (q->rd != q->wt) {
		q->rd = (q->rd + 1) % q->num;
		q->rd_cnt++;
	}
}

static int tx_fifo_init(struct sc2331_tx_fifo *f)
{
	f->raw_size = SC2331_TX_FIFO_SIZE;
	f->raw_mem = alloc_pages_exact(f->raw_size, GFP_KERNEL);
	if (!f->raw_mem)
		return -ENOMEM;
	memset(f->raw_mem, 0, f->raw_size);
	f->mem = f->raw_mem + sizeof(struct sc2331_tx_big_hdr);
	f->size = f->raw_size - sizeof(struct sc2331_tx_big_hdr) - SC2331_SDIO_ALIGN_SIZE;
	f->cp2_tx_ram = SC2331_HW_TX_SIZE - sizeof(struct sc2331_tx_big_hdr);
	f->max_msg_num = SC2331_PKT_AGGR_NUM;
	f->wt = f->rd = f->seq = 0;
	f->last_wt = -1;
	spin_lock_init(&f->lock);
	return 0;
}

static void tx_fifo_destroy(struct sc2331_tx_fifo *f)
{
	if (f->raw_mem) {
		free_pages_exact(f->raw_mem, f->raw_size);
		f->raw_mem = NULL;
		f->mem = NULL;
	}
}

static u32 tx_fifo_used_bytes(struct sc2331_tx_fifo *f)
{
	unsigned long flags;
	u32 wt, rd, used;
	int last_wt;

	spin_lock_irqsave(&f->lock, flags);
	wt = f->wt;
	rd = f->rd;
	last_wt = f->last_wt;
	spin_unlock_irqrestore(&f->lock, flags);

	if (last_wt < 0)
		used = wt - rd;
	else
		used = (last_wt - rd) + wt;
	return used;
}

static int tx_fifo_push(struct sc2331_tx_fifo *f, const struct sc2331_tx_msg *msg)
{
	u32 unit = SC2331_TX_UNIT_LEN(&msg->hdr);
	struct sc2331_msg_hdr *dst_hdr;
	u32 rd = READ_ONCE(f->rd);
	unsigned long flags;
	u8 *payload_dst;

	if (f->wt >= rd) {
		if (unit > f->size - f->wt) {
			if (unit + sizeof(struct sc2331_tx_big_hdr) > rd)
				return -ENOSPC;
			spin_lock_irqsave(&f->lock, flags);
			f->last_wt = f->wt;
			f->wt = 0;
			spin_unlock_irqrestore(&f->lock, flags);
		}
	} else {
		if (unit + sizeof(struct sc2331_tx_big_hdr) > rd - f->wt)
			return -ENOSPC;
	}

	dst_hdr = (struct sc2331_msg_hdr *)(f->mem + f->wt);
	*dst_hdr = msg->hdr;
	if (msg->hdr.type == HOST_SC2331_PKT) {
		memset(dst_hdr + 1, 0, 34);
		memcpy(dst_hdr + 1, &f->seq, 4);
		f->seq++;
	}
	payload_dst = (u8 *)dst_hdr + SC2331_TX_HDR_SPACE(dst_hdr);
	if (msg->data && msg->data_len)
		memcpy(payload_dst, msg->data, msg->data_len);

	smp_wmb();
	WRITE_ONCE(f->wt, f->wt + unit);
	return 0;
}

static int tx_fifo_flush_to_sdio(struct sc2331_wlan *w, u8 vif_id, u32 chn,
				 struct sc2331_tx_fifo *f)
{
	u8 *read_from, *read_to, *read_max;
	struct sc2331_tx_big_hdr *big;
	unsigned long flags;
	u32 wt, len, padded;
	int num, ret;

restart:
	wt = READ_ONCE(f->wt);
	if (f->rd == wt) {
		if (f->last_wt < 0)
			return 0;
		read_max = f->mem + f->last_wt;
	} else if (wt < f->rd) {
		if ((int)f->rd == f->last_wt) {
			spin_lock_irqsave(&f->lock, flags);
			f->rd = 0;
			f->last_wt = -1;
			spin_unlock_irqrestore(&f->lock, flags);
			goto restart;
		}
		read_max = f->mem + f->last_wt;
	} else {
		read_max = f->mem + wt;
	}

	read_to = f->mem + f->rd;
	read_from = read_to - sizeof(struct sc2331_tx_big_hdr);
	big = (struct sc2331_tx_big_hdr *)read_from;
	memset(big, 0, sizeof(*big));
	big->mode = vif_id;
	len = sizeof(*big);

	for (num = 0; num < f->max_msg_num; num++) {
		struct sc2331_msg_hdr *h = (struct sc2331_msg_hdr *)read_to;
		u32 unit;

		if (read_to >= read_max)
			break;
		unit = SC2331_TX_UNIT_LEN(h);
		if (len + unit >= f->cp2_tx_ram)
			break;
		big->msg[num] = *h;
		big->msg_num++;
		len += unit;
		read_to += unit;
	}

	if (!num)
		return 0;

	big->len = (u16)(read_to - read_from);
	big->tx_cnt = w->tx_cnt;
	padded = (big->len + 1023U) & ~1023U;

	ret = sc2331_sdio_write(chn, read_from, padded);
	if (ret) {
		dev_err(w->dev, "TX SDIO write chn %u (%u B) failed (%d)\n",
			chn, padded, ret);
		return ret;
	}
	w->tx_cnt++;

	spin_lock_irqsave(&f->lock, flags);
	if (f->rd > wt && (size_t)(read_max - read_to) < sizeof(struct sc2331_msg_hdr)) {
		f->rd = 0;
		f->last_wt = -1;
	} else {
		f->rd = (u32)(read_to - f->mem);
	}
	spin_unlock_irqrestore(&f->lock, flags);
	return num;
}

static int rx_fifo_init(struct sc2331_rx_fifo *f)
{
	int i;

	memset(f, 0, sizeof(*f));
	for (i = 0; i < SC2331_RX_FIFO_BLOCKS; i++) {
		f->blocks[i] = kmalloc(SC2331_HW_RX_SIZE, GFP_KERNEL);
		if (!f->blocks[i])
			return -ENOMEM;
	}
	return 0;
}

static void rx_fifo_destroy(struct sc2331_rx_fifo *f)
{
	int i;

	for (i = 0; i < SC2331_RX_FIFO_BLOCKS; i++) {
		kfree(f->blocks[i]);
		f->blocks[i] = NULL;
	}
}

static bool rx_fifo_is_full(struct sc2331_rx_fifo *f)
{
	return ((f->wt + 1) % SC2331_RX_FIFO_BLOCKS) == f->rd;
}

/* =========================================================================
 * Command RPC & Event Dispatch
 * ========================================================================= */

static void sc2331_wake_core(struct sc2331_wlan *w)
{
	up(&w->core_sem);
}

static void sc2331_wake_trans(struct sc2331_wlan *w)
{
	up(&w->trans_sem);
}

static void sc2331_rx_chn_isr(int chn)
{
	if (g_wlan)
		sc2331_wake_trans(g_wlan);
}

static int sc2331_send_cmd_async(struct sc2331_wlan *w, u8 vif_id,
				 u8 subtype, void *payload, u16 len)
{
	struct sc2331_tx_msg msg = {};
	int ret;

	msg.hdr.mode = vif_id;
	msg.hdr.type = HOST_SC2331_CMD;
	msg.hdr.subtype = subtype;
	msg.hdr.len = payload ? len : 0;
	msg.data = payload;
	msg.data_len = payload ? len : 0;

	ret = msg_q_push(&w->vif.cmd_q, &msg);
	if (ret) {
		kfree(payload);
		return ret;
	}
	sc2331_wake_core(w);
	return 0;
}

static int sc2331_send_cmd_sync(struct sc2331_wlan *w, u8 vif_id,
				u8 subtype, void *payload, u16 len,
				void *rsp_out, u16 *rsp_len_io,
				unsigned int timeout_ms)
{
	struct sc2331_msg_hdr *rsp_hdr;
	unsigned long rem;
	int ret;

	if (atomic_read(&w->exit_flag)) {
		kfree(payload);
		return -ENODEV;
	}

	mutex_lock(&w->cmd_lock);

	/* Clear any stale response */
	mutex_lock(&w->rsp_lock);
	w->rsp_ready = 0;
	mutex_unlock(&w->rsp_lock);

	ret = sc2331_send_cmd_async(w, vif_id, subtype, payload, len);
	if (ret) {
		mutex_unlock(&w->cmd_lock);
		return ret;
	}

	rem = wait_event_timeout(w->rsp_wq,
				 w->rsp_ready || atomic_read(&w->exit_flag),
				 msecs_to_jiffies(timeout_ms));
	if (!rem || atomic_read(&w->exit_flag)) {
		dev_err(w->dev, "cmd %u timeout\n", subtype);
		mutex_unlock(&w->cmd_lock);
		return -ETIMEDOUT;
	}

	mutex_lock(&w->rsp_lock);
	w->rsp_ready = 0;
	rsp_hdr = (struct sc2331_msg_hdr *)w->rsp_buf;
	if (rsp_hdr->type != SC2331_HOST_RSP || rsp_hdr->subtype != subtype) {
		dev_err(w->dev, "cmd %u rsp mismatch (type=%u sub=%u)\n",
			subtype, rsp_hdr->type, rsp_hdr->subtype);
		ret = -EPROTO;
	} else {
		if (rsp_out && rsp_len_io) {
			u16 copy_len = min_t(u16, *rsp_len_io, rsp_hdr->len);

			memcpy(rsp_out, w->rsp_buf + sizeof(*rsp_hdr), copy_len);
			*rsp_len_io = copy_len;
		}
		ret = 0;
	}
	mutex_unlock(&w->rsp_lock);
	mutex_unlock(&w->cmd_lock);
	return ret;
}

static int sc2331_cmd_simple_u32(struct sc2331_wlan *w, u8 sub, u32 val)
{
	struct sc2331_cmd_u32 *p = kmalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return -ENOMEM;
	p->val = val;
	return sc2331_send_cmd_sync(w, 0, sub, p, sizeof(*p),
				    NULL, NULL, SC2331_CMD_TIMEOUT_MS);
}

static int sc2331_cmd_mac_open(struct sc2331_wlan *w, u8 mode, const u8 *mac)
{
	struct sc2331_cmd_mac_open *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return -ENOMEM;
	p->mode = mode;
	memcpy(p->mac, mac, ETH_ALEN);
	sc2331_set_wlan_status(1);
	return sc2331_send_cmd_sync(w, 0, WIFI_CMD_SET_DEV_OPEN, p, sizeof(*p),
				    NULL, NULL, 8000);
}

static int sc2331_cmd_mac_close(struct sc2331_wlan *w, u8 mode)
{
	struct sc2331_cmd_mac_close *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return -ENOMEM;
	p->mode = mode;
	sc2331_set_wlan_status(0);
	return sc2331_send_cmd_sync(w, 0, WIFI_CMD_SET_DEV_CLOSE, p, sizeof(*p),
				    NULL, NULL, SC2331_CMD_TIMEOUT_MS);
}

/* =========================================================================
 * Scan & Connect/Disconnect event handlers
 * ========================================================================= */

static void sc2331_finish_scan(struct sc2331_vif *vif, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };

	if (!vif->scan_req)
		return;
	if (!atomic_add_unless(&vif->scan_lock, 1, 1))
		return;

	if (vif->scan_req) {
		timer_delete(&vif->scan_timer);
		cfg80211_scan_done(vif->scan_req, &info);
		vif->scan_req = NULL;
	}
	atomic_dec(&vif->scan_lock);
}

static void sc2331_scan_timer_fn(struct timer_list *t)
{
	struct sc2331_vif *vif = timer_container_of(vif, t, scan_timer);

	sc2331_finish_scan(vif, true);
}

static void sc2331_inform_frame_bss(struct wiphy *wiphy, u16 chan_num,
				    s16 rssi_dbm, const u8 *msa, u16 msa_len)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)msa;
	struct ieee80211_channel *chan;
	struct cfg80211_bss *bss;
	enum cfg80211_bss_frame_type ftype = CFG80211_BSS_FTYPE_UNKNOWN;
	int freq;
	size_t hdr_len = offsetof(struct ieee80211_mgmt, u.probe_resp.variable);

	if (msa_len <= hdr_len)
		return;

	freq = ieee80211_channel_to_frequency(chan_num, NL80211_BAND_2GHZ);
	chan = ieee80211_get_channel(wiphy, freq);
	if (!chan)
		return;

	if (ieee80211_is_beacon(mgmt->frame_control))
		ftype = CFG80211_BSS_FTYPE_BEACON;
	else if (ieee80211_is_probe_resp(mgmt->frame_control))
		ftype = CFG80211_BSS_FTYPE_PRESP;

	bss = cfg80211_inform_bss(wiphy, chan, ftype,
				  mgmt->bssid,
				  le64_to_cpu(mgmt->u.probe_resp.timestamp),
				  le16_to_cpu(mgmt->u.probe_resp.capab_info),
				  le16_to_cpu(mgmt->u.probe_resp.beacon_int),
				  mgmt->u.probe_resp.variable,
				  msa_len - hdr_len,
				  (s32)rssi_dbm * 100,
				  GFP_KERNEL);
	if (bss)
		cfg80211_put_bss(wiphy, bss);
}

static void sc2331_handle_scan_frame_event(struct sc2331_wlan *w,
					   const u8 *data, u16 len)
{
	struct sc2331_vif *vif = &w->vif;
	const struct sc2331_event_scan_rsp *ev;
	int i;

	if (len < sizeof(*ev))
		return;
	ev = (const struct sc2331_event_scan_rsp *)data;
	if (sizeof(*ev) + ev->frame_len > len)
		return;

	if (ev->ops == 0) {
		const u8 *msa = data + sizeof(*ev);
		const u8 *bssid;

		if (ev->frame_len < 37 || ev->frame_len > sizeof(vif->scan_bufs[0].msa))
			return;
		bssid = msa + 16;

		/* Update existing BSSID or store in free slot */
		for (i = 0; i < SC2331_MAX_SCAN_BUFS; i++) {
			struct sc2331_scan_entry *e = &vif->scan_bufs[i];

			if (e->live == 0xff && ether_addr_equal(e->bssid, bssid)) {
				memcpy(e->msa, msa, ev->frame_len);
				e->msa_len = ev->frame_len;
				e->channel = ev->channel;
				e->signal = ev->signal;
				e->keep = 1;
				return;
			}
		}
		for (i = 0; i < SC2331_MAX_SCAN_BUFS; i++) {
			struct sc2331_scan_entry *e = &vif->scan_bufs[i];

			if (e->live != 0xff) {
				memcpy(e->bssid, bssid, ETH_ALEN);
				memcpy(e->msa, msa, ev->frame_len);
				e->msa_len = ev->frame_len;
				e->channel = ev->channel;
				e->signal = ev->signal;
				e->keep = 1;
				e->live = 0xff;
				return;
			}
		}
		return;
	}

	if (ev->ops == 1) {
		for (i = 0; i < SC2331_MAX_SCAN_BUFS; i++) {
			struct sc2331_scan_entry *e = &vif->scan_bufs[i];

			if (e->live != 0xff)
				continue;
			if (!e->keep) {
				memset(e, 0, sizeof(*e));
				continue;
			}
			e->keep--;
			sc2331_inform_frame_bss(w->wiphy, e->channel, e->signal,
						e->msa, e->msa_len);
		}
		sc2331_finish_scan(vif, false);
	}
}

static void sc2331_handle_scandone_event(struct sc2331_wlan *w,
					 const u8 *pos, u16 len)
{
	u32 left = len;

	while (left >= 10) {
		u16 ch_len, ch = 0, rssi_len, mgmt_len;
		s16 rssi = 0;

		ch_len = get_unaligned_le16(pos);
		pos += 2;
		left -= 2;
		if (ch_len > 2 || ch_len > left)
			break;
		memcpy(&ch, pos, ch_len);
		pos += ch_len;
		left -= ch_len;

		if (left < 2)
			break;
		rssi_len = get_unaligned_le16(pos);
		pos += 2;
		left -= 2;
		if (rssi_len > 2 || rssi_len > left)
			break;
		memcpy(&rssi, pos, rssi_len);
		pos += rssi_len;
		left -= rssi_len;

		if (left < 2)
			break;
		mgmt_len = get_unaligned_le16(pos);
		pos += 2;
		left -= 2;
		if (mgmt_len > left)
			break;

		sc2331_inform_frame_bss(w->wiphy, ch, rssi, pos, mgmt_len);
		pos += mgmt_len;
		left -= mgmt_len;
	}
	sc2331_finish_scan(&w->vif, false);
}

static void sc2331_notify_disconnected(struct sc2331_wlan *w, u16 reason,
				       bool locally_generated)
{
	struct sc2331_vif *vif = &w->vif;
	enum sc2331_conn_state prev;

	spin_lock(&vif->state_lock);
	prev = vif->conn_state;
	vif->conn_state = SC2331_DISCONNECTED;
	spin_unlock(&vif->state_lock);

	if (prev == SC2331_CONNECTING) {
		cfg80211_connect_result(vif->ndev, vif->bssid, NULL, 0,
					NULL, 0, WLAN_STATUS_UNSPECIFIED_FAILURE,
					GFP_KERNEL);
	} else if (prev == SC2331_CONNECTED) {
		cfg80211_disconnected(vif->ndev, reason, NULL, 0,
				      locally_generated, GFP_KERNEL);
	}
	if (netif_carrier_ok(vif->ndev)) {
		netif_carrier_off(vif->ndev);
		netif_stop_queue(vif->ndev);
	}
	if (prev != SC2331_DISCONNECTED)
		dev_info(w->dev, "disconnected (%s, reason=%u)\n",
			 locally_generated ? "local" : "remote", reason);
}

static void sc2331_handle_connect_event(struct sc2331_wlan *w,
					const u8 *data, u16 len)
{
	struct sc2331_vif *vif = &w->vif;
	const u8 *bssid_ptr, *req_ie_ptr, *resp_ie_ptr;
	u8 status_code, req_ie_len;
	u16 bssid_len, resp_ie_len;
	bool was_connecting = false;
	int left;

	if (len < 3 || data[0] != 1)
		goto fail;

	status_code = data[2];
	if (status_code != 0) {
		dev_info(w->dev, "connect failed (status=%u)\n", status_code);
		goto fail;
	}

	left = len - 3;
	if (left < 8)
		goto fail;

	bssid_len = get_unaligned_le16(data + 3);
	left -= 2;
	if (bssid_len != ETH_ALEN || left < bssid_len + 1)
		goto fail;
	bssid_ptr = data + 5;
	left -= bssid_len;

	req_ie_len = bssid_ptr[bssid_len];
	left -= 1;
	if (left < req_ie_len + 2)
		goto fail;
	req_ie_ptr = bssid_ptr + bssid_len + 1;
	left -= req_ie_len;

	resp_ie_len = get_unaligned_le16(req_ie_ptr + req_ie_len);
	left -= 2;
	resp_ie_ptr = req_ie_ptr + req_ie_len + 2;
	if (resp_ie_len > left)
		resp_ie_len = left;

	memcpy(vif->bssid, bssid_ptr, ETH_ALEN);
	spin_lock(&vif->state_lock);
	if (vif->conn_state == SC2331_CONNECTING) {
		vif->conn_state = SC2331_CONNECTED;
		was_connecting = true;
	}
	spin_unlock(&vif->state_lock);

	if (was_connecting) {
		cfg80211_connect_result(vif->ndev, bssid_ptr,
					req_ie_ptr, req_ie_len,
					resp_ie_ptr, resp_ie_len,
					WLAN_STATUS_SUCCESS, GFP_KERNEL);
		if (!netif_carrier_ok(vif->ndev)) {
			netif_carrier_on(vif->ndev);
			netif_wake_queue(vif->ndev);
		}
		dev_info(w->dev, "connected to %pM\n", bssid_ptr);
	}
	return;

fail:
	sc2331_finish_scan(vif, true);
	sc2331_notify_disconnected(w, WLAN_REASON_UNSPECIFIED, false);
}

static void sc2331_handle_disconnect_event(struct sc2331_wlan *w,
					   const u8 *data, u16 len)
{
	struct sc2331_vif *vif = &w->vif;
	u16 reason = len >= 2 ? get_unaligned_le16(data) : WLAN_REASON_UNSPECIFIED;

	sc2331_finish_scan(vif, true);
	sc2331_notify_disconnected(w, reason, false);
}

static void sc2331_process_rx_block(struct sc2331_wlan *w, u8 *buf, u32 max_len)
{
	u32 pos = 0;

	if (max_len <= 8)
		return;
	buf += 8;
	max_len -= 8;

	while (pos + sizeof(struct sc2331_msg_hdr) <= max_len) {
		struct sc2331_msg_hdr *hdr = (struct sc2331_msg_hdr *)(buf + pos);
		u8 *pdata = (u8 *)(hdr + 1);
		u16 len = hdr->len;

		if (hdr->type == 0x7f || hdr->subtype == 0xff)
			break;
		if (pos + sizeof(*hdr) + len > max_len)
			break;

		if (hdr->type == HOST_SC2331_PKT) {
			u16 skip = hdr->subtype;

			if (len > skip) {
				struct sk_buff *skb = dev_alloc_skb(len - skip + NET_IP_ALIGN);

				if (skb) {
					skb_reserve(skb, NET_IP_ALIGN);
					memcpy(skb_put(skb, len - skip), pdata + skip, len - skip);
					skb->dev = w->vif.ndev;
					skb->protocol = eth_type_trans(skb, w->vif.ndev);
					w->vif.ndev->stats.rx_packets++;
					w->vif.ndev->stats.rx_bytes += skb->len;
					netif_rx(skb);
				}
			}
		} else if (hdr->type == SC2331_HOST_RSP) {
			mutex_lock(&w->rsp_lock);
			if (sizeof(*hdr) + len <= 8192) {
				memcpy(w->rsp_buf, hdr, sizeof(*hdr) + len);
				w->rsp_ready = 1;
				wake_up(&w->rsp_wq);
			}
			mutex_unlock(&w->rsp_lock);
		} else if (hdr->type == HOST_SC2331_CMD) {
			switch (hdr->subtype) {
			case WIFI_EVENT_CONNECT:
				sc2331_handle_connect_event(w, pdata, len);
				break;
			case WIFI_EVENT_DISCONNECT:
				sc2331_handle_disconnect_event(w, pdata, len);
				break;
			case WIFI_EVENT_SCANDONE:
				sc2331_handle_scandone_event(w, pdata, len);
				break;
			case WIFI_EVENT_REPORT_SCAN_FRAME:
				sc2331_handle_scan_frame_event(w, pdata, len);
				break;
			default:
				break;
			}
		} else {
			break;
		}

		pos += sizeof(*hdr) + ALIGN(len, 4);
	}
}

/* =========================================================================
 * Core & Transport Kernel Threads
 * ========================================================================= */

static int sc2331_core_thread(void *arg)
{
	struct sc2331_wlan *w = arg;
	struct sc2331_vif *vif = &w->vif;
	struct sc2331_rx_fifo *rxf = &w->rx_fifo;

	while (!kthread_should_stop() && !atomic_read(&w->exit_flag)) {
		bool did_work = false;
		struct sc2331_tx_msg *m;

		/* Drain RX blocks */
		while (rxf->rd != READ_ONCE(rxf->wt)) {
			sc2331_process_rx_block(w, rxf->blocks[rxf->rd],
						rxf->lens[rxf->rd]);
			smp_mb();
			WRITE_ONCE(rxf->rd, (rxf->rd + 1) % SC2331_RX_FIFO_BLOCKS);
			did_work = true;
		}

		/* Drain CMD queue first */
		while ((m = msg_q_peek(&vif->cmd_q)) != NULL) {
			if (tx_fifo_push(&vif->tx_fifo, m))
				break;
			kfree(m->data);
			msg_q_pop(&vif->cmd_q);
			sc2331_wake_trans(w);
			did_work = true;
		}

		/* Drain DATA queue */
		while ((m = msg_q_peek(&vif->data_q)) != NULL) {
			if (tx_fifo_push(&vif->tx_fifo, m))
				break;
			if (m->skb)
				dev_kfree_skb_any(m->skb);
			msg_q_pop(&vif->data_q);
			sc2331_wake_trans(w);
			did_work = true;
		}

		if (!did_work) {
			if (down_timeout(&w->core_sem, msecs_to_jiffies(200)))
				continue;
		}
	}
	return 0;
}

static void sc2331_drain_non_wifi_chn(struct sc2331_wlan *w, u32 chn)
{
	int len = sc2331_sdio_get_chn_datalen(chn);
	u8 *tmp;

	if (len <= 0 || len > 16384)
		return;
	tmp = kmalloc(len + 1, GFP_KERNEL);
	if (tmp) {
		u32 rlen = 0;

		if (!sc2331_sdio_read(chn, tmp, &rlen) && rlen > 0) {
			u32 n = min_t(u32, rlen, 160);
			u32 i;
			int printable = 0;

			for (i = 0; i < n; i++) {
				if (tmp[i] >= 0x20 && tmp[i] <= 0x7e)
					printable++;
				else if (tmp[i] == '\r' || tmp[i] == '\n')
					tmp[i] = ' ';
				else
					tmp[i] = '.';
			}
			tmp[n] = '\0';
			if (printable >= 4)
				dev_info_ratelimited(w->dev, "CP2 ch%u (%uB): %s\n",
						     chn, rlen, (char *)tmp);
		}
		kfree(tmp);
	}
}

static int sc2331_trans_thread(void *arg)
{
	struct sc2331_wlan *w = arg;
	struct sc2331_rx_fifo *rxf = &w->rx_fifo;
	struct sc2331_tx_fifo *txf = &w->vif.tx_fifo;
	static const u8 tx_chns[3] = { 0, 1, 2 };
	static const u8 rx_chns[6] = { 8, 9, 14, 11, 15, 13 };

	sc2331_sdio_register_rx_cb(SC2331_WIFI_CHN_RX_8, sc2331_rx_chn_isr);
	sc2331_sdio_register_rx_cb(SC2331_WIFI_CHN_RX_9, sc2331_rx_chn_isr);

	while (!kthread_should_stop() && !atomic_read(&w->exit_flag)) {
		bool did_work = false;
		u16 status = 0;
		int i;

		/* 1. RX path when data_irq GPIO is high */
		if (sc2331_get_rx_gpio_val()) {
			if (!sc2331_marlin_wakeup(0, 1) &&
			    !sc2331_sdio_chn_status(0xeb00, &status)) {
				for (i = 0; i < ARRAY_SIZE(rx_chns); i++) {
					u8 chn = rx_chns[i];

					if (!(status & BIT(chn)))
						continue;
					if (chn == 8 || chn == 9) {
						if (!rx_fifo_is_full(rxf)) {
							u32 rlen = 0;

							if (!sc2331_sdio_read(chn, rxf->blocks[rxf->wt], &rlen)) {
								rxf->lens[rxf->wt] = min_t(u32, rlen, SC2331_HW_RX_SIZE);
								smp_wmb();
								WRITE_ONCE(rxf->wt, (rxf->wt + 1) % SC2331_RX_FIFO_BLOCKS);
								w->rx_cnt++;
								sc2331_wake_core(w);
								did_work = true;
							}
						}
					} else {
						sc2331_drain_non_wifi_chn(w, chn);
						did_work = true;
					}
					break;
				}
			}
		}

		/* 2. TX path when tx_fifo has queued data */
		if (tx_fifo_used_bytes(txf) > 0) {
			int wret = sc2331_marlin_wakeup(0, 1);

			if (!wret && !sc2331_sdio_chn_status(0x0007, &status)) {
				u16 free_mask = (status & 0x0007) ^ 0x0007;

				for (i = 0; i < ARRAY_SIZE(tx_chns); i++) {
					if (free_mask & BIT(tx_chns[i])) {
						if (tx_fifo_flush_to_sdio(w, 0, tx_chns[i], txf) > 0) {
							did_work = true;
							sc2331_wake_core(w);
						}
						break;
					}
				}
			} else {
				if (wret)
					dev_warn_ratelimited(w->dev, "TX wakeup err %d\n", wret);
				usleep_range(1000, 2000);
			}
		}

		if (!did_work && !sc2331_get_rx_gpio_val() && tx_fifo_used_bytes(txf) == 0) {
			if (down_timeout(&w->trans_sem, msecs_to_jiffies(200)))
				continue;
		} else if (!did_work) {
			usleep_range(500, 1000);
		}
	}

	sc2331_sdio_unregister_rx_cb(SC2331_WIFI_CHN_RX_8);
	sc2331_sdio_unregister_rx_cb(SC2331_WIFI_CHN_RX_9);
	return 0;
}

/* =========================================================================
 * cfg80211_ops (Linux 7.3.1)
 * ========================================================================= */

static int sc2331_ensure_sta_open(struct sc2331_wlan *w, struct sc2331_vif *vif)
{
	int ret;

	if (vif->mode == SC2331_MODE_STATION)
		return 0;

	ret = sc2331_cmd_mac_open(w, SC2331_MODE_STATION, vif->ndev->dev_addr);
	if (ret) {
		dev_err(w->dev, "WIFI_CMD_SET_DEV_OPEN failed (%d)\n", ret);
		return ret;
	}
	vif->mode = SC2331_MODE_STATION;
	dev_info(w->dev, "MAC opened in STATION mode (%pM)\n", vif->ndev->dev_addr);
	return 0;
}

static int sc2331_cfg_change_iface(struct wiphy *wiphy, struct net_device *ndev,
				   enum nl80211_iftype type,
				   struct vif_params *params)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(ndev);

	if (type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;
	vif->wdev.iftype = type;
	if (netif_running(ndev))
		return sc2331_ensure_sta_open(g_wlan, vif);
	return 0;
}

static int sc2331_cfg_scan(struct wiphy *wiphy,
			   struct cfg80211_scan_request *req)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(req->wdev->netdev);
	struct sc2331_wlan *w = g_wlan;
	u8 channels[16] = {};
	u8 *ssid_buf = NULL, *cmd_buf;
	int ssid_len = 0, i, j = 0, n, ret;
	u32 cmd_len;

	ret = sc2331_ensure_sta_open(w, vif);
	if (ret)
		return ret;

	if (vif->scan_req)
		return -EBUSY;

	if (req->ie_len > 0 && req->ie_len <= 255) {
		struct sc2331_cmd_wps_ie *wps = kmalloc(sizeof(*wps) + req->ie_len, GFP_KERNEL);

		if (wps) {
			wps->type = 1; /* WPS_REQ_IE */
			wps->len = req->ie_len;
			memcpy(wps->value, req->ie, req->ie_len);
			sc2331_send_cmd_sync(w, 0, WIFI_CMD_SET_WPS_IE,
					     wps, sizeof(*wps) + req->ie_len,
					     NULL, NULL, SC2331_CMD_TIMEOUT_MS);
		}
	}

	n = min_t(int, req->n_ssids, 9);
	if (n > 0) {
		ssid_buf = kzalloc(512, GFP_KERNEL);
		if (!ssid_buf)
			return -ENOMEM;
		for (i = 0; i < n; i++) {
			struct sc2331_cmd_scan_ssid *s;

			if (!req->ssids[i].ssid_len)
				continue;
			if (ssid_len + 1 + req->ssids[i].ssid_len > 512)
				break;
			s = (struct sc2331_cmd_scan_ssid *)(ssid_buf + ssid_len);
			s->len = req->ssids[i].ssid_len;
			memcpy(s->ssid, req->ssids[i].ssid, s->len);
			ssid_len += 1 + s->len;
		}
	}

	n = min_t(int, req->n_channels, 14);
	for (i = 0; i < n; i++) {
		u8 ch = req->channels[i]->hw_value;

		if (ch >= 1 && ch <= 14)
			channels[++j] = ch;
	}
	channels[0] = j;

	cmd_len = (j + 1) + sizeof(struct sc2331_cmd_scan_tail) + ssid_len;
	cmd_buf = kzalloc(cmd_len, GFP_KERNEL);
	if (!cmd_buf) {
		kfree(ssid_buf);
		return -ENOMEM;
	}
	memcpy(cmd_buf, channels, j + 1);
	put_unaligned_le32(ssid_len, cmd_buf + j + 1);
	if (ssid_len)
		memcpy(cmd_buf + j + 1 + 4, ssid_buf, ssid_len);
	kfree(ssid_buf);

	vif->scan_req = req;
	mod_timer(&vif->scan_timer, jiffies + msecs_to_jiffies(SC2331_SCAN_TIMEOUT_MS));

	ret = sc2331_send_cmd_sync(w, 0, WIFI_CMD_SET_SCAN, cmd_buf, cmd_len,
				   NULL, NULL, SC2331_CMD_TIMEOUT_MS);
	if (ret) {
		timer_delete_sync(&vif->scan_timer);
		vif->scan_req = NULL;
		return ret;
	}
	return 0;
}

static u32 sc2331_map_cipher(u32 suite)
{
	switch (suite) {
	case WLAN_CIPHER_SUITE_WEP40:
		return SC2331_CIPHER_WEP40;
	case WLAN_CIPHER_SUITE_WEP104:
		return SC2331_CIPHER_WEP104;
	case WLAN_CIPHER_SUITE_TKIP:
		return SC2331_CIPHER_TKIP;
	case WLAN_CIPHER_SUITE_CCMP:
		return SC2331_CIPHER_CCMP;
	default:
		return SC2331_CIPHER_NONE;
	}
}

static int sc2331_cfg_connect(struct wiphy *wiphy, struct net_device *ndev,
			      struct cfg80211_connect_params *sme)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(ndev);
	struct sc2331_wlan *w = g_wlan;
	bool is_wep = (sme->crypto.cipher_group == WLAN_CIPHER_SUITE_WEP40 ||
		       sme->crypto.cipher_group == WLAN_CIPHER_SUITE_WEP104);
	u32 auth_type = SC2331_AUTH_OPEN;
	u32 cipher;
	int ret;

	ret = sc2331_ensure_sta_open(w, vif);
	if (ret)
		return ret;

	dev_info(w->dev, "connecting to '%.*s' bssid=%pM wpa=0x%x auth=%u grp=0x%x pw=0x%x akm=0x%x\n",
		 (int)sme->ssid_len, sme->ssid ? (const char *)sme->ssid : "",
		 sme->bssid ? sme->bssid : (const u8 *)"\0\0\0\0\0\0",
		 sme->crypto.wpa_versions, sme->auth_type,
		 sme->crypto.cipher_group,
		 sme->crypto.n_ciphers_pairwise ? sme->crypto.ciphers_pairwise[0] : 0,
		 sme->crypto.n_akm_suites ? sme->crypto.akm_suites[0] : 0);

	/* WPA version */
	ret = sc2331_cmd_simple_u32(w, WIFI_CMD_SET_WPA_VERSION,
				    sme->crypto.wpa_versions);
	if (ret)
		return ret;

	/* Auth type */
	if (sme->auth_type == NL80211_AUTHTYPE_SHARED_KEY ||
	    (sme->auth_type == NL80211_AUTHTYPE_AUTOMATIC && is_wep))
		auth_type = SC2331_AUTH_SHARED;
	ret = sc2331_cmd_simple_u32(w, WIFI_CMD_SET_AUTH_TYPE, auth_type);
	if (ret)
		return ret;

	/* Pairwise cipher */
	if (sme->crypto.n_ciphers_pairwise) {
		cipher = sc2331_map_cipher(sme->crypto.ciphers_pairwise[0]);
		ret = sc2331_cmd_simple_u32(w, WIFI_CMD_SET_PAIRWISE_CIPHER, cipher);
		if (ret)
			return ret;
	}

	/* Group cipher */
	cipher = sc2331_map_cipher(sme->crypto.cipher_group);
	ret = sc2331_cmd_simple_u32(w, WIFI_CMD_SET_GROUP_CIPHER, cipher);
	if (ret)
		return ret;

	/* Re-send auth type (CP2 firmware quirk from vendor driver) */
	sc2331_cmd_simple_u32(w, WIFI_CMD_SET_AUTH_TYPE, auth_type);

	/* AKM suite */
	if (sme->crypto.n_akm_suites) {
		u32 akm = 0;

		switch (sme->crypto.akm_suites[0]) {
		case WLAN_AKM_SUITE_PSK:
			akm = SC2331_AKM_PSK;
			break;
		case WLAN_AKM_SUITE_8021X:
			akm = SC2331_AKM_8021X;
			break;
		case WLAN_AKM_SUITE_FT_PSK:
			akm = SC2331_AKM_FT_PSK;
			break;
		case WLAN_AKM_SUITE_FT_8021X:
			akm = SC2331_AKM_FT_8021X;
			break;
		default:
			break;
		}
		ret = sc2331_cmd_simple_u32(w, WIFI_CMD_SET_AKM_SUITE, akm);
		if (ret)
			return ret;
	}

	/* PSK or WEP key */
	if (is_wep && sme->key && sme->key_len > 0) {
		struct sc2331_cmd_add_key *k = kzalloc(sizeof(*k) + sme->key_len, GFP_KERNEL);

		if (k) {
			k->cypher_type = sc2331_map_cipher(sme->crypto.cipher_group);
			k->key_index = sme->key_idx;
			k->key_len = sme->key_len;
			k->pairwise = 0;
			memcpy(k->value, sme->key, sme->key_len);
			sc2331_send_cmd_sync(w, 0, WIFI_CMD_KEY_ADD,
					     k, sizeof(*k) + sme->key_len,
					     NULL, NULL, SC2331_CMD_TIMEOUT_MS);
		}
	} else {
		u16 klen = min_t(u16, sme->key_len, 32);
		struct sc2331_cmd_psk *psk = kzalloc(sizeof(*psk) + klen, GFP_KERNEL);

		if (psk) {
			psk->len = klen;
			if (klen && sme->key)
				memcpy(psk->key, sme->key, klen);
			sc2331_send_cmd_sync(w, 0, WIFI_CMD_SET_PSK,
					     psk, sizeof(*psk) + klen,
					     NULL, NULL, SC2331_CMD_TIMEOUT_MS);
		}
	}

	if (sme->channel) {
		u32 ch = ieee80211_frequency_to_channel(sme->channel->center_freq);

		sc2331_cmd_simple_u32(w, WIFI_CMD_SET_CHANNEL, ch);
	}

	if (sme->bssid) {
		struct sc2331_cmd_bssid *b = kmalloc(sizeof(*b), GFP_KERNEL);

		if (b) {
			memcpy(b->addr, sme->bssid, ETH_ALEN);
			sc2331_send_cmd_sync(w, 0, WIFI_CMD_SET_BSSID, b, sizeof(*b),
					     NULL, NULL, SC2331_CMD_TIMEOUT_MS);
		}
		memcpy(vif->bssid, sme->bssid, ETH_ALEN);
	}

	if (is_wep && sme->key_len > 0)
		sc2331_cmd_simple_u32(w, WIFI_CMD_KEY_SET, sme->key_idx);

	if (sme->ssid && sme->ssid_len > 0) {
		struct sc2331_cmd_essid *e = kmalloc(sizeof(*e) + sme->ssid_len, GFP_KERNEL);

		if (!e)
			return -ENOMEM;
		e->len = sme->ssid_len;
		memcpy(e->essid, sme->ssid, sme->ssid_len);
		memcpy(vif->ssid, sme->ssid, min_t(size_t, sme->ssid_len, sizeof(vif->ssid)));
		vif->ssid_len = sme->ssid_len;
		spin_lock(&vif->state_lock);
		vif->conn_state = SC2331_CONNECTING;
		spin_unlock(&vif->state_lock);

		ret = sc2331_send_cmd_sync(w, 0, WIFI_CMD_SET_ESSID,
					   e, sizeof(*e) + sme->ssid_len,
					   NULL, NULL, SC2331_CMD_TIMEOUT_MS);
		if (ret) {
			spin_lock(&vif->state_lock);
			vif->conn_state = SC2331_DISCONNECTED;
			spin_unlock(&vif->state_lock);
			return ret;
		}
	}
	return 0;
}

static int sc2331_cfg_disconnect(struct wiphy *wiphy, struct net_device *ndev,
				 u16 reason_code)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(ndev);
	struct sc2331_cmd_u16 *p;
	int ret;

	if (vif->mode == SC2331_MODE_NONE)
		return 0;

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->val = reason_code;
	ret = sc2331_send_cmd_sync(g_wlan, 0, WIFI_CMD_SET_DISCONNECT,
				   p, sizeof(*p), NULL, NULL, SC2331_CMD_TIMEOUT_MS);
	memset(vif->ssid, 0, sizeof(vif->ssid));
	vif->ssid_len = 0;
	/*
	 * CP2 firmware does not reliably emit WIFI_EVENT_DISCONNECT for
	 * host-initiated disconnect commands; notify cfg80211 immediately
	 * so wdev->connected is cleared and subsequent reconnects succeed.
	 */
	sc2331_notify_disconnected(g_wlan, reason_code, true);
	return ret;
}

static int sc2331_cfg_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			      int link_id, u8 key_index, bool pairwise,
			      const u8 *mac_addr, struct key_params *params)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(wdev->netdev);
	struct sc2331_cmd_add_key *k;
	u8 cipher = sc2331_map_cipher(params->cipher);

	if (vif->mode == SC2331_MODE_NONE)
		return -EAGAIN;
	if (!cipher || !params->key || !params->key_len)
		return -EINVAL;

	k = kzalloc(sizeof(*k) + params->key_len, GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	if (mac_addr)
		memcpy(k->mac, mac_addr, ETH_ALEN);
	if (params->seq && params->seq_len)
		memcpy(k->keyseq, params->seq, min_t(int, params->seq_len, 8));
	k->pairwise = pairwise ? 1 : 0;
	k->cypher_type = cipher;
	k->key_index = key_index;
	k->key_len = params->key_len;
	memcpy(k->value, params->key, params->key_len);

	return sc2331_send_cmd_sync(g_wlan, 0, WIFI_CMD_KEY_ADD,
				    k, sizeof(*k) + params->key_len,
				    NULL, NULL, SC2331_CMD_TIMEOUT_MS);
}

static int sc2331_cfg_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			      int link_id, u8 key_index, bool pairwise,
			      const u8 *mac_addr)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(wdev->netdev);
	struct sc2331_cmd_del_key *k;

	if (vif->mode == SC2331_MODE_NONE)
		return 0;

	k = kzalloc(sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	k->key_index = key_index;
	k->pairwise = pairwise ? 1 : 0;
	if (mac_addr)
		memcpy(k->mac, mac_addr, ETH_ALEN);

	return sc2331_send_cmd_sync(g_wlan, 0, WIFI_CMD_KEY_DEL, k, sizeof(*k),
				    NULL, NULL, SC2331_CMD_TIMEOUT_MS);
}

static int sc2331_cfg_set_default_key(struct wiphy *wiphy,
				      struct net_device *ndev, int link_id,
				      u8 key_index, bool unicast, bool multicast)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(ndev);

	if (vif->mode == SC2331_MODE_NONE)
		return -EAGAIN;
	return sc2331_cmd_simple_u32(g_wlan, WIFI_CMD_KEY_SET, key_index);
}

static int sc2331_cfg_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				  const u8 *mac, struct station_info *sinfo)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(wdev->netdev);
	u8 rbuf[12] = {};
	u16 rlen;

	if (vif->mode == SC2331_MODE_NONE)
		return -EAGAIN;

	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BYTES) |
			 BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_RX_BYTES) |
			 BIT_ULL(NL80211_STA_INFO_RX_PACKETS);
	sinfo->tx_bytes = vif->ndev->stats.tx_bytes;
	sinfo->tx_packets = vif->ndev->stats.tx_packets;
	sinfo->rx_bytes = vif->ndev->stats.rx_bytes;
	sinfo->rx_packets = vif->ndev->stats.rx_packets;

	rlen = 8;
	if (!sc2331_send_cmd_sync(g_wlan, 0, WIFI_CMD_GET_RSSI, NULL, 0,
				  rbuf, &rlen, SC2331_CMD_TIMEOUT_MS) && rlen >= 8) {
		u32 raw = get_unaligned_le32(rbuf + 4);

		sinfo->signal = (s8)(raw & 0xff);
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
	}
	return 0;
}

static const struct cfg80211_ops sc2331_cfg80211_ops = {
	.change_virtual_intf	= sc2331_cfg_change_iface,
	.scan			= sc2331_cfg_scan,
	.connect		= sc2331_cfg_connect,
	.disconnect		= sc2331_cfg_disconnect,
	.add_key		= sc2331_cfg_add_key,
	.del_key		= sc2331_cfg_del_key,
	.set_default_key	= sc2331_cfg_set_default_key,
	.get_station		= sc2331_cfg_get_station,
};

/* =========================================================================
 * net_device_ops & IPv4 notifier
 * ========================================================================= */

static int sc2331_net_open(struct net_device *ndev)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(ndev);
	int ret;

	ret = sc2331_ensure_sta_open(g_wlan, vif);
	if (ret)
		return ret;
	netif_start_queue(ndev);
	return 0;
}

static int sc2331_net_stop(struct net_device *ndev)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(ndev);

	netif_stop_queue(ndev);
	sc2331_finish_scan(vif, true);
	/*
	 * Do NOT send WIFI_CMD_SET_DEV_CLOSE on link-down (matches vendor
	 * wlan_close): CP2 firmware expects MAC_OPEN once and stalls if
	 * closed/reopened repeatedly by NetworkManager link toggles.
	 */
	return 0;
}

static netdev_tx_t sc2331_net_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct sc2331_vif *vif = ndev_to_sc2331_vif(ndev);
	struct sc2331_tx_msg msg = {};

	if (vif->mode == SC2331_MODE_NONE) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	msg.hdr.mode = vif->id;
	msg.hdr.type = HOST_SC2331_PKT;
	msg.hdr.subtype = 0;
	msg.hdr.len = skb->len;
	msg.data = skb->data;
	msg.data_len = skb->len;
	msg.skb = skb;

	if (msg_q_push(&vif->data_q, &msg)) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	netif_trans_update(ndev);
	sc2331_wake_core(g_wlan);
	return NETDEV_TX_OK;
}

static void sc2331_net_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	netif_wake_queue(ndev);
	if (g_wlan)
		sc2331_wake_core(g_wlan);
}

static const struct net_device_ops sc2331_netdev_ops = {
	.ndo_open		= sc2331_net_open,
	.ndo_stop		= sc2331_net_stop,
	.ndo_start_xmit		= sc2331_net_xmit,
	.ndo_tx_timeout		= sc2331_net_tx_timeout,
};

static int sc2331_inetaddr_cb(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = ptr;
	struct sc2331_wlan *w = container_of(nb, struct sc2331_wlan, inetaddr_nb);

	if (!ifa->ifa_dev || ifa->ifa_dev->dev != w->vif.ndev)
		return NOTIFY_DONE;

	if (event == NETDEV_UP && w->vif.mode != SC2331_MODE_NONE) {
		struct sc2331_cmd_get_ip *p = kmalloc(sizeof(*p), GFP_KERNEL);

		if (p) {
			memcpy(p->ip, &ifa->ifa_address, 4);
			sc2331_send_cmd_async(w, 0, WIFI_CMD_GET_IP, p, sizeof(*p));
		}
	}
	return NOTIFY_DONE;
}

static void sc2331_load_mac_addr(struct device *dev, u8 *mac)
{
	const struct firmware *fw = NULL;
	unsigned int b[6];

	if (!firmware_request_nowarn(&fw, "sprd/wifi_mac.txt", dev)) {
		char buf[32] = {};

		memcpy(buf, fw->data, min_t(size_t, fw->size, sizeof(buf) - 1));
		release_firmware(fw);
		if (sscanf(buf, "%x:%x:%x:%x:%x:%x",
			   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
			int i;

			for (i = 0; i < ETH_ALEN; i++)
				mac[i] = (u8)b[i];
			if (is_valid_ether_addr(mac)) {
				dev_info(dev, "loaded factory Wi-Fi MAC %pM\n", mac);
				return;
			}
		}
	}
	eth_random_addr(mac);
	dev_info(dev, "using generated Wi-Fi MAC %pM\n", mac);
}

/* =========================================================================
 * Stage-3 Entry / Exit (called from sc2331_boot.c when Marlin is ready)
 * ========================================================================= */

int sc2331_wlan_start(struct device *dev)
{
	struct sc2331_wlan *w;
	struct wiphy *wiphy;
	struct net_device *ndev;
	u8 mac[ETH_ALEN];
	int ret;

	if (g_wlan)
		return 0;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	w->dev = dev;
	spin_lock_init(&w->vif.state_lock);
	mutex_init(&w->cmd_lock);
	mutex_init(&w->rsp_lock);
	init_waitqueue_head(&w->rsp_wq);
	sema_init(&w->trans_sem, 0);
	sema_init(&w->core_sem, 0);
	atomic_set(&w->exit_flag, 0);

	w->rsp_buf = kmalloc(8192, GFP_KERNEL);
	w->vif.scan_bufs = kcalloc(SC2331_MAX_SCAN_BUFS,
				   sizeof(*w->vif.scan_bufs), GFP_KERNEL);
	if (!w->rsp_buf || !w->vif.scan_bufs) {
		ret = -ENOMEM;
		goto err_mem;
	}

	ret = msg_q_init(&w->vif.cmd_q, 16);
	if (ret)
		goto err_mem;
	ret = msg_q_init(&w->vif.data_q, 160);
	if (ret)
		goto err_mem;
	ret = tx_fifo_init(&w->vif.tx_fifo);
	if (ret)
		goto err_mem;
	ret = rx_fifo_init(&w->rx_fifo);
	if (ret)
		goto err_mem;

	wiphy = wiphy_new(&sc2331_cfg80211_ops, 0);
	if (!wiphy) {
		ret = -ENOMEM;
		goto err_mem;
	}
	w->wiphy = wiphy;
	set_wiphy_dev(wiphy, dev);
	sc2331_load_mac_addr(dev, mac);
	memcpy(wiphy->perm_addr, mac, ETH_ALEN);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->max_scan_ssids = 9;
	wiphy->max_scan_ie_len = 255;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->cipher_suites = sc2331_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(sc2331_ciphers);
	wiphy->bands[NL80211_BAND_2GHZ] = &sc2331_band_2ghz;
	wiphy->flags &= ~WIPHY_FLAG_PS_ON_BY_DEFAULT;

	ret = wiphy_register(wiphy);
	if (ret) {
		dev_err(dev, "wiphy_register failed (%d)\n", ret);
		goto err_wiphy;
	}

	ndev = alloc_netdev(sizeof(void *), "wlan%d", NET_NAME_ENUM, ether_setup);
	if (!ndev) {
		ret = -ENOMEM;
		goto err_unreg_wiphy;
	}

	w->vif.id = 0;
	w->vif.ndev = ndev;
	w->vif.mode = SC2331_MODE_NONE;
	w->vif.conn_state = SC2331_DISCONNECTED;
	w->vif.wdev.wiphy = wiphy;
	w->vif.wdev.iftype = NL80211_IFTYPE_STATION;
	w->vif.wdev.netdev = ndev;
	atomic_set(&w->vif.scan_lock, 0);
	timer_setup(&w->vif.scan_timer, sc2331_scan_timer_fn, 0);

	*(struct sc2331_vif **)netdev_priv(ndev) = &w->vif;
	ndev->netdev_ops = &sc2331_netdev_ops;
	ndev->ieee80211_ptr = &w->vif.wdev;
	ndev->watchdog_timeo = 5 * HZ;
	SET_NETDEV_DEV(ndev, dev);

	eth_hw_addr_set(ndev, mac);

	g_wlan = w;

	w->trans_task = kthread_run(sc2331_trans_thread, w, "sc2331_trans");
	if (IS_ERR(w->trans_task)) {
		ret = PTR_ERR(w->trans_task);
		w->trans_task = NULL;
		goto err_ndev;
	}

	w->core_task = kthread_run(sc2331_core_thread, w, "sc2331_core");
	if (IS_ERR(w->core_task)) {
		ret = PTR_ERR(w->core_task);
		w->core_task = NULL;
		goto err_threads;
	}

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(dev, "register_netdev failed (%d)\n", ret);
		goto err_threads;
	}
	netif_carrier_off(ndev);

	w->inetaddr_nb.notifier_call = sc2331_inetaddr_cb;
	register_inetaddr_notifier(&w->inetaddr_nb);

	dev_info(dev, "sc2331: %s registered (%pM)\n", ndev->name, mac);
	return 0;

err_threads:
	atomic_set(&w->exit_flag, 1);
	if (w->core_task) {
		sc2331_wake_core(w);
		kthread_stop(w->core_task);
	}
	if (w->trans_task) {
		sc2331_wake_trans(w);
		kthread_stop(w->trans_task);
	}
err_ndev:
	g_wlan = NULL;
	free_netdev(ndev);
err_unreg_wiphy:
	wiphy_unregister(wiphy);
err_wiphy:
	wiphy_free(wiphy);
err_mem:
	rx_fifo_destroy(&w->rx_fifo);
	tx_fifo_destroy(&w->vif.tx_fifo);
	msg_q_destroy(&w->vif.data_q);
	msg_q_destroy(&w->vif.cmd_q);
	kfree(w->vif.scan_bufs);
	kfree(w->rsp_buf);
	kfree(w);
	return ret;
}

void sc2331_wlan_stop(struct device *dev)
{
	struct sc2331_wlan *w = g_wlan;

	if (!w)
		return;

	unregister_inetaddr_notifier(&w->inetaddr_nb);
	if (w->vif.mode != SC2331_MODE_NONE)
		sc2331_cmd_mac_close(w, w->vif.mode);

	atomic_set(&w->exit_flag, 1);
	wake_up_all(&w->rsp_wq);
	timer_delete_sync(&w->vif.scan_timer);

	if (w->vif.ndev) {
		unregister_netdev(w->vif.ndev);
		free_netdev(w->vif.ndev);
	}
	if (w->core_task) {
		sc2331_wake_core(w);
		kthread_stop(w->core_task);
	}
	if (w->trans_task) {
		sc2331_wake_trans(w);
		kthread_stop(w->trans_task);
	}
	if (w->wiphy) {
		wiphy_unregister(w->wiphy);
		wiphy_free(w->wiphy);
	}

	rx_fifo_destroy(&w->rx_fifo);
	tx_fifo_destroy(&w->vif.tx_fifo);
	msg_q_destroy(&w->vif.data_q);
	msg_q_destroy(&w->vif.cmd_q);
	kfree(w->vif.scan_bufs);
	kfree(w->rsp_buf);
	kfree(w);
	g_wlan = NULL;
}
