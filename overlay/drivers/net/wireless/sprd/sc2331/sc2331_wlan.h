/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SC2331_WLAN_H_
#define _SC2331_WLAN_H_

#include <linux/ieee80211.h>
#include <linux/netdevice.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <net/cfg80211.h>

#define SC2331_SDIO_ALIGN_SIZE		1024
#define SC2331_HW_TX_SIZE		13312
#define SC2331_HW_RX_SIZE		12288
#define SC2331_PKT_AGGR_NUM		12
#define SC2331_RX_FIFO_BLOCKS		8
#define SC2331_TX_FIFO_SIZE		(256 * 1024)
#define SC2331_CMD_TIMEOUT_MS		3000
#define SC2331_SCAN_TIMEOUT_MS		10000
#define SC2331_MAX_SCAN_BUFS		40

enum sc2331_msg_type {
	HOST_SC2331_CMD = 0,
	SC2331_HOST_RSP = 1,
	HOST_SC2331_PKT = 2,
	HOST_SC2331_WAPI = 3,
};

enum sc2331_cmd_id {
	WIFI_CMD_GET_MODE = 1,
	WIFI_CMD_GET_RSSI,
	WIFI_CMD_GET_TXRATE_TXFAILED,
	WIFI_CMD_SET_SCAN,
	WIFI_CMD_SET_AUTH_TYPE,
	WIFI_CMD_SET_WPA_VERSION,
	WIFI_CMD_SET_PAIRWISE_CIPHER,
	WIFI_CMD_SET_GROUP_CIPHER,
	WIFI_CMD_SET_AKM_SUITE,
	WIFI_CMD_SET_CHANNEL,
	WIFI_CMD_SET_BSSID,
	WIFI_CMD_SET_ESSID,
	WIFI_CMD_KEY_ADD,
	WIFI_CMD_KEY_DEL,
	WIFI_CMD_KEY_SET,
	WIFI_CMD_SET_DISCONNECT,
	WIFI_CMD_SET_RTS_THRESHOLD,
	WIFI_CMD_SET_FRAG_THRESHOLD,
	WIFI_CMD_SET_PMKSA,
	WIFI_CMD_DEL_PMKSA,
	WIFI_CMD_FLUSH_PMKSA,
	WIFI_CMD_SET_DEV_OPEN,
	WIFI_CMD_SET_DEV_CLOSE,
	WIFI_CMD_SET_PSK,
	WIFI_CMD_START_BEACON,
	WIFI_CMD_SET_WPS_IE,
	WIFI_CMD_TX_MGMT,
	WIFI_CMD_REMAIN_CHAN,
	WIFI_CMD_CANCEL_REMAIN_CHAN,
	WIFI_CMD_P2P_IE,
	WIFI_CMD_CHANGE_BEACON,
	WIFI_CMD_REGISTER_FRAME,
	WIFI_CMD_NPI_MSG,
	WIFI_CMD_NPI_GET,
	WIFI_CMD_SET_FT_IE,
	WIFI_CMD_UPDATE_FT_IE,
	WIFI_CMD_ASSERT,
	WIFI_CMD_SLEEP,
	WIFI_CMD_ADD_SOFTAP_BLACKLIST,
	WIFI_CMD_DEL_SOFTAP_BLACKLIST,
	WIFI_CMD_SCAN_NOR_CHANNELS,
	WIFI_CMD_GET_IP,
	WIFI_CMD_REQ_LTE_CONCUR,
	WIFI_CMD_SET_CQM_RSSI,
	WIFI_CMD_MULTICAST_FILTER,
	WIFI_CMD_DISASSOC,
	WIFI_CMD_MAX,

	WIFI_EVENT_CONNECT = 128,
	WIFI_EVENT_DISCONNECT,
	WIFI_EVENT_SCANDONE,
	WIFI_EVENT_MGMT_DEAUTH,
	WIFI_EVENT_MGMT_DISASSOC,
	WIFI_EVENT_REMAIN_ON_CHAN_EXPIRED,
	WIFI_EVENT_NEW_STATION,
	WIFI_EVENT_REPORT_FRAME,
	WIFI_EVENT_CONNECT_AP,
	WIFI_EVENT_SDIO_SEQ_NUM,
	WIFI_EVENT_REPORT_SCAN_FRAME,
	WIFI_EVENT_REPORT_MIC_FAIL,
	WIFI_EVENT_REPORT_CQM_RSSI_LOW,
	WIFI_EVENT_REPORT_CQM_RSSI_HIGH,
	WIFI_EVENT_REPORT_CQM_RSSI_LOSS_BEACON,
	WIFI_EVENT_MLME_TX_STATUS,
};

enum sc2331_wlan_mode {
	SC2331_MODE_NONE = 0,
	SC2331_MODE_STATION = 1,
	SC2331_MODE_AP = 2,
};

enum sc2331_cipher {
	SC2331_CIPHER_NONE = 0,
	SC2331_CIPHER_WEP40 = 1,
	SC2331_CIPHER_WEP104 = 2,
	SC2331_CIPHER_TKIP = 3,
	SC2331_CIPHER_CCMP = 4,
};

enum sc2331_akm {
	SC2331_AKM_PSK = 1,
	SC2331_AKM_8021X = 2,
	SC2331_AKM_FT_8021X = 3,
	SC2331_AKM_FT_PSK = 4,
};

enum sc2331_auth {
	SC2331_AUTH_OPEN = 0,
	SC2331_AUTH_SHARED = 1,
};

enum sc2331_conn_state {
	SC2331_DISCONNECTED = 0,
	SC2331_CONNECTING,
	SC2331_CONNECTED,
};

/* Wire headers (exact match with SC2331 CP2 firmware) */
struct sc2331_msg_hdr {
	u8 type:7;
	u8 mode:1;
	u8 subtype;
	u16 len;
};

struct sc2331_tx_big_hdr {
	u8 mode;
	u8 msg_num;
	u16 len;
	struct sc2331_msg_hdr msg[14];
	u32 tx_cnt;
};

#define SC2331_TX_HDR_SPACE(h) \
	(sizeof(struct sc2331_msg_hdr) + (((h)->type == HOST_SC2331_PKT) ? 34 : 0))
#define SC2331_TX_UNIT_LEN(h) \
	ALIGN(SC2331_TX_HDR_SPACE(h) + (h)->len, 4)

struct sc2331_cmd_mac_open {
	u16 mode;
	u8 mac[ETH_ALEN];
} __packed;

struct sc2331_cmd_mac_close {
	u8 mode;
} __packed;

struct sc2331_cmd_scan_tail {
	u32 len;
	u8 ssid[];
} __packed;

struct sc2331_cmd_scan_ssid {
	u8 len;
	u8 ssid[];
} __packed;

struct sc2331_cmd_u32 {
	u32 val;
} __packed;

struct sc2331_cmd_u16 {
	u16 val;
} __packed;

struct sc2331_cmd_bssid {
	u8 addr[ETH_ALEN];
} __packed;

struct sc2331_cmd_essid {
	u16 len;
	u8 essid[];
} __packed;

struct sc2331_cmd_psk {
	u16 len;
	u8 key[];
} __packed;

struct sc2331_cmd_add_key {
	u8 mac[ETH_ALEN];
	u8 keyseq[8];
	u8 pairwise;
	u8 cypher_type;
	u8 key_index;
	u8 key_len;
	u8 value[];
} __packed;

struct sc2331_cmd_del_key {
	u8 key_index;
	u8 pairwise;
	u8 mac[ETH_ALEN];
} __packed;

struct sc2331_cmd_get_ip {
	u8 ip[4];
} __packed;

struct sc2331_cmd_wps_ie {
	u8 type;
	u8 len;
	u8 value[];
} __packed;

/* Naturally aligned (8 bytes total) to match vendor CP2 firmware */
struct sc2331_event_scan_rsp {
	u8 ops;
	u16 channel;
	s16 signal;
	u16 frame_len;
};

struct sc2331_scan_entry {
	u8 live;
	u8 keep;
	u16 channel;
	s16 signal;
	u16 msa_len;
	u8 bssid[ETH_ALEN];
	u8 msa[1024];
};

#endif /* _SC2331_WLAN_H_ */
