/*
 * drivers/video/tegra/dc/edid_quirks.c
 *
 * Copyright (c) 2015, NVIDIA CORPORATION, All rights reserved.
 * Author: Anshuman Nath Kar <anshumank@nvidia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "edid.h"

static const struct hdmi_blacklist {
	char manufacturer[4];
	u32 model;
	char monitor[14];
	u32 quirks;
} edid_blacklist[] = {
	/* Bauhn ATVS65-815 65" 4K TV */
	{ "CTV", 48,    "Tempo 4K TV",   TEGRA_EDID_QUIRK_NO_YUV },
	/* Samsung JS8500 */
	{ "SAM", 3077,  "SAMSUNG",       TEGRA_EDID_QUIRK_370_BUMP_DOWN },
	/* 138.5 MHz pclk rounding bug */
	{ "LEN", 4420,  "",              TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	{ "LEN", 4420,  "LEN LT2452pwC", TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	{ "SAM", 1179,  "SyncMaster",    TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	{ "GSM", 22255, "W2353",         TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	{ "GSM", 22266, "W2361",         TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	{ "MJI", 23,    "SR6004 HDMI2",  TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	{ "LEN", 4420,  "",              TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	{ "GSM", 22236, "W2253",         TEGRA_EDID_QUIRK_FIX_138_50_MHZ },
	/* 138.65 MHz pclk rounding bug */
	{ "AW_", 22084, "PULSE Serie",   TEGRA_EDID_QUIRK_FIX_138_65_MHZ },
	{ "AW_", 18757, "SQD Series",    TEGRA_EDID_QUIRK_FIX_138_65_MHZ },
};

u32 tegra_edid_lookup_quirks(const char *manufacturer, u32 model,
	const char *monitor)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(edid_blacklist); i++)
		if (!strcmp(edid_blacklist[i].manufacturer, manufacturer) &&
			edid_blacklist[i].model == model &&
			!strcmp(edid_blacklist[i].monitor, monitor))
			return edid_blacklist[i].quirks;

	return TEGRA_EDID_QUIRK_NONE;
}
