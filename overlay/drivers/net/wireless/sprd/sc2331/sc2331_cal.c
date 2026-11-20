// SPDX-License-Identifier: GPL-2.0
/*
 * SC2331 (Marlin 1) INI configuration & RF calibration parser.
 * Packs connectivity_configure.ini (offset 0) and
 * connectivity_calibration.ini (offset 616) into the 4096-byte binary
 * struct expected by SC2331 firmware on SDIO channel 0.
 */

#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "sc2331_sdio.h"
#include "sc2331_cal_tables.h"

static char *skip_ws(char *s)
{
	while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
		s++;
	return s;
}

static void trim_right(char *s)
{
	size_t len = strlen(s);

	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
			   s[len - 1] == '\r' || s[len - 1] == '\n'))
		s[--len] = '\0';
}

static const struct sc2331_ini_field *
find_field(const struct sc2331_ini_field *tbl, size_t n, const char *key,
	   bool *w16_seen)
{
	size_t i;

	/* Match exact name first (case-insensitive for rf_init_num/RF_init_num) */
	for (i = 0; i < n; i++) {
		if (!strcasecmp(tbl[i].name, key)) {
			if (!strcmp(key, "reserved_w16_regs") && w16_seen) {
				if (*w16_seen)
					continue;
				*w16_seen = true;
			}
			return &tbl[i];
		}
	}

	/* Stock INI uses reserved_w32_regs; stock table entry at 192 was named reserved_w16_regs */
	if (!strcmp(key, "reserved_w32_regs")) {
		for (i = 0; i < n; i++) {
			if (tbl[i].offset == 192 && !strcmp(tbl[i].name, "reserved_w16_regs"))
				return &tbl[i];
		}
	}
	return NULL;
}

static int parse_ini_into_buf(struct device *dev, const u8 *ini_data,
			      size_t ini_size,
			      const struct sc2331_ini_field *tbl,
			      size_t tbl_len, u8 *dst, size_t dst_max)
{
	char *copy, *p, *line;
	bool w16_seen = false;
	int matched = 0;

	copy = kmalloc(ini_size + 1, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	memcpy(copy, ini_data, ini_size);
	copy[ini_size] = '\0';

	p = copy;
	while ((line = strsep(&p, "\n")) != NULL) {
		const struct sc2331_ini_field *f;
		char *hash, *eq, *key, *vals, *tok;
		u32 off;

		hash = strchr(line, '#');
		if (hash)
			*hash = '\0';

		line = skip_ws(line);
		trim_right(line);
		if (!*line || *line == '[')
			continue;

		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		key = skip_ws(line);
		trim_right(key);
		vals = skip_ws(eq + 1);

		f = find_field(tbl, tbl_len, key, &w16_seen);
		if (!f)
			continue;

		off = f->offset;
		while ((tok = strsep(&vals, ",")) != NULL) {
			long val;
			int ret;

			tok = skip_ws(tok);
			trim_right(tok);
			if (!*tok)
				continue;

			ret = kstrtol(tok, 0, &val);
			if (ret) {
				unsigned long uval;

				ret = kstrtoul(tok, 0, &uval);
				if (ret)
					continue;
				val = (long)uval;
			}

			if (off + f->elem_size > dst_max)
				break;

			switch (f->elem_size) {
			case 1:
				dst[off] = (u8)val;
				break;
			case 2:
				dst[off] = (u8)(val & 0xff);
				dst[off + 1] = (u8)((val >> 8) & 0xff);
				break;
			case 4:
				dst[off] = (u8)(val & 0xff);
				dst[off + 1] = (u8)((val >> 8) & 0xff);
				dst[off + 2] = (u8)((val >> 16) & 0xff);
				dst[off + 3] = (u8)((val >> 24) & 0xff);
				break;
			default:
				break;
			}
			off += f->elem_size;
		}
		matched++;
	}

	kfree(copy);
	return matched;
}

int sc2331_build_cali_buf(struct device *dev, u8 *buf, size_t buf_size)
{
	const struct firmware *cfg_fw = NULL, *cal_fw = NULL;
	int ret, n_cfg, n_cal;

	if (buf_size < SC2331_CALI_BUF_SIZE)
		return -EINVAL;

	memset(buf, 0, SC2331_CALI_BUF_SIZE);

	ret = request_firmware(&cfg_fw, "sprd/connectivity_configure.ini", dev);
	if (ret) {
		dev_err(dev, "failed to load sprd/connectivity_configure.ini (%d)\n", ret);
		return ret;
	}

	n_cfg = parse_ini_into_buf(dev, cfg_fw->data, cfg_fw->size,
				   sc2331_cfg_fields,
				   ARRAY_SIZE(sc2331_cfg_fields),
				   buf, SC2331_CALI_DATA_OFFSET);
	release_firmware(cfg_fw);
	if (n_cfg < 0)
		return n_cfg;

	ret = request_firmware(&cal_fw, "sprd/connectivity_calibration.ini", dev);
	if (ret) {
		dev_err(dev, "failed to load sprd/connectivity_calibration.ini (%d)\n", ret);
		return ret;
	}

	n_cal = parse_ini_into_buf(dev, cal_fw->data, cal_fw->size,
				   sc2331_cal_fields,
				   ARRAY_SIZE(sc2331_cal_fields),
				   buf + SC2331_CALI_DATA_OFFSET,
				   SC2331_CALI_BUF_SIZE - SC2331_CALI_DATA_OFFSET);
	release_firmware(cal_fw);
	if (n_cal < 0)
		return n_cal;

	dev_info(dev, "parsed RF config (%d keys) + calibration (%d keys, is_calibrated=%u)\n",
		 n_cfg, n_cal, buf[SC2331_CALI_DATA_OFFSET + 1]);
	return 0;
}
