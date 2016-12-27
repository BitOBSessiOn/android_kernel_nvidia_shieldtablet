/*
 * drivers/video/tegra/dc/hdmi2.0.c
 *
 * Copyright (c) 2014-2016, NVIDIA CORPORATION, All rights reserved.
 * Author: Animesh Kishore <ankishore@nvidia.com>
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

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/clk/tegra.h>
#include <linux/nvhost.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/unistd.h>
#ifdef CONFIG_SWITCH
#include <linux/switch.h>
#endif
#include <linux/of_address.h>
#include <linux/tegra_pm_domains.h>

#include <mach/dc.h>
#include <mach/fb.h>
#include <mach/powergate.h>

#include "dc_reg.h"
#include "dc_priv.h"
#include "sor.h"
#include "sor_regs.h"
#include "edid.h"
#include "hdmi2.0.h"
#include "hdmihdcp.h"
#include "dpaux.h"
#ifdef CONFIG_ADF_TEGRA
#include "tegra_adf.h"
#endif
#include "hda_dc.h"
#include "hdmivrr.h"

#include <linux/tegra_prod.h>
#include "../../../../arch/arm/mach-tegra/iomap.h"

#define TMDS_NODE	"/host1x/sor1"

#ifdef CONFIG_PM_GENERIC_DOMAINS_OF
static struct of_device_id tegra_sor_pd[] = {
	{ .compatible = "nvidia, tegra210-sor-pd", },
	{ .compatible = "nvidia, tegra132-sor-pd", },
	{ .compatible = "nvidia, tegra124-sor-pd", },
	{},
};
#endif

static ssize_t hdmi_ddc_power_toggle(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count);

static ssize_t hdmi_ddc_power_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);

static struct kobj_attribute hdmi_ddc_power_config =
	__ATTR(config, 0640, hdmi_ddc_power_show, hdmi_ddc_power_toggle);

static struct kobject *hdmi_ddc;

struct tmds_prod_pair {
	int clk;
	const char *name;
};

static struct tmds_prod_pair tmds_config_modes[] = {
	{ /* 54 MHz */
	.clk = 54000000,
	.name = "prod_c_54M"
	},
	{ /* 75 MHz */
	.clk = 75000000,
	.name = "prod_c_75M"
	},
	{ /* 150 MHz */
	.clk = 150000000,
	.name = "prod_c_150M"
	},
	{ /* 300 MHz */
	.clk = 300000000,
	.name = "prod_c_300M"
	},
	{ /* HDMI 2.0 */
	.clk = 600000000,
	.name = "prod_c_600M"
	}
};

static struct tegra_hdmi *dc_hdmi;

static int tegra_hdmi_controller_enable(struct tegra_hdmi *hdmi);
static void tegra_hdmi_config_clk(struct tegra_hdmi *hdmi, u32 clk_type);
static long tegra_dc_hdmi_setup_clk(struct tegra_dc *dc, struct clk *clk);
static void tegra_hdmi_scdc_worker(struct work_struct *work);
static void tegra_hdmi_debugfs_init(struct tegra_hdmi *hdmi);
static void tegra_hdmi_hdr_worker(struct work_struct *work);
static int tegra_hdmi_v2_x_mon_config(struct tegra_hdmi *hdmi, bool enable);
static void tegra_hdmi_v2_x_host_config(struct tegra_hdmi *hdmi, bool enable);

static inline bool tegra_hdmi_is_connected(struct tegra_hdmi *hdmi)
{
	return (hdmi->mon_spec.misc & FB_MISC_HDMI) ||
		(hdmi->mon_spec.misc & FB_MISC_HDMI_FORUM);
}

static inline void tegra_hdmi_irq_enable(struct tegra_hdmi *hdmi)
{
	if (tegra_platform_is_fpga())
		return;

	enable_irq(hdmi->irq);
}

static inline void tegra_hdmi_irq_disable(struct tegra_hdmi *hdmi)
{
	if (tegra_platform_is_fpga())
		return;

	disable_irq(hdmi->irq);
}

static inline bool tegra_hdmi_hpd_asserted(struct tegra_hdmi *hdmi)
{
	return tegra_dc_hpd(hdmi->dc);
}

static inline void tegra_hdmi_reset(struct tegra_hdmi *hdmi)
{
	if (tegra_platform_is_linsim())
		return;

	tegra_periph_reset_assert(hdmi->sor->sor_clk);
	udelay(10);
	tegra_periph_reset_deassert(hdmi->sor->sor_clk);
	udelay(10);
}

static inline void _tegra_hdmi_ddc_enable(struct tegra_hdmi *hdmi)
{
	mutex_lock(&hdmi->ddc_refcount_lock);
	if (hdmi->ddc_refcount++)
		goto fail;
	tegra_hdmi_get(hdmi->dc);
	/*
	 * hdmi uses i2c lane muxed on dpaux1 pad.
	 * Enable dpaux1 pads and configure the mux.
	 */
	tegra_dpaux_config_pad_mode(hdmi->dc, TEGRA_DPAUX_INSTANCE_1,
					TEGRA_DPAUX_PAD_MODE_I2C);

fail:
	mutex_unlock(&hdmi->ddc_refcount_lock);
}

static inline void _tegra_hdmi_ddc_disable(struct tegra_hdmi *hdmi)
{
	mutex_lock(&hdmi->ddc_refcount_lock);

	if (WARN_ONCE(hdmi->ddc_refcount <= 0, "ddc refcount imbalance"))
		goto fail;
	if (--hdmi->ddc_refcount != 0)
		goto fail;

	/*
	 * hdmi uses i2c lane muxed on dpaux1 pad.
	 * Disable dpaux1 pads.
	 */
	tegra_dpaux_pad_power(hdmi->dc, TEGRA_DPAUX_INSTANCE_1, false);
	tegra_hdmi_put(hdmi->dc);

fail:
	mutex_unlock(&hdmi->ddc_refcount_lock);
}

static int tegra_hdmi_ddc_i2c_xfer(struct tegra_dc *dc,
					struct i2c_msg *msgs, int num)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);
	int ret;

	_tegra_hdmi_ddc_enable(hdmi);
	ret = i2c_transfer(hdmi->ddc_i2c_client->adapter, msgs, num);
	_tegra_hdmi_ddc_disable(hdmi);
	return ret;
}

static int tegra_hdmi_ddc_init(struct tegra_hdmi *hdmi, int edid_src)
{
	struct tegra_dc *dc = hdmi->dc;
	struct i2c_adapter *i2c_adap;
	int err = 0;
	struct i2c_board_info i2c_dev_info = {
		.type = "tegra_hdmi2.0",
		.addr = 0x50,
	};
	if (edid_src == 0)
		hdmi->edid = tegra_edid_create(dc, tegra_hdmi_ddc_i2c_xfer);
	else if (edid_src == 1)
		hdmi->edid = tegra_edid_create(dc, tegra_dc_edid_blob);
	if (IS_ERR_OR_NULL(hdmi->edid)) {
		dev_err(&dc->ndev->dev, "hdmi: can't create edid\n");
		return PTR_ERR(hdmi->edid);
	}
	tegra_dc_set_edid(dc, hdmi->edid);

	i2c_adap = i2c_get_adapter(dc->out->ddc_bus);
	if (!i2c_adap) {
		dev_err(&dc->ndev->dev,
			"hdmi: can't get adpater for ddc bus %d\n",
			dc->out->ddc_bus);
		err = -EBUSY;
		goto fail_edid_free;
	}
	hdmi->ddc_i2c_original_rate = i2c_get_adapter_bus_clk_rate(i2c_adap);

	hdmi->ddc_i2c_client = i2c_new_device(i2c_adap, &i2c_dev_info);
	i2c_put_adapter(i2c_adap);
	if (!hdmi->ddc_i2c_client) {
		dev_err(&dc->ndev->dev, "hdmi: can't create new i2c device\n");
		err = -EBUSY;
		goto fail_edid_free;
	}

	return 0;
fail_edid_free:
	tegra_edid_destroy(hdmi->edid);
	return err;
}

static int tegra_hdmi_scdc_i2c_xfer(struct tegra_dc *dc,
					struct i2c_msg *msgs, int num)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	return i2c_transfer(hdmi->scdc_i2c_client->adapter, msgs, num);
}

static int tegra_hdmi_scdc_init(struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;
	struct i2c_adapter *i2c_adap;
	int err = 0;
	struct i2c_board_info i2c_dev_info = {
		.type = "tegra_hdmi_scdc",
		.addr = 0x54,
	};

	i2c_adap = i2c_get_adapter(dc->out->ddc_bus);
	if (!i2c_adap) {
		dev_err(&dc->ndev->dev,
			"hdmi: can't get adpater for scdc bus %d\n",
			dc->out->ddc_bus);
		err = -EBUSY;
		goto fail;
	}

	hdmi->scdc_i2c_client = i2c_new_device(i2c_adap, &i2c_dev_info);
	i2c_put_adapter(i2c_adap);
	if (!hdmi->scdc_i2c_client) {
		dev_err(&dc->ndev->dev,
			"hdmi: can't create scdc i2c device\n");
		err = -EBUSY;
		goto fail;
	}

	INIT_DELAYED_WORK(&hdmi->scdc_work, tegra_hdmi_scdc_worker);

	return 0;
fail:
	return err;
}

/*  does not return precise tmds character rate */
static u32 tegra_hdmi_mode_min_tmds_rate(const struct fb_videomode *mode)
{
	u32 tmds_csc_8bpc_khz = PICOS2KHZ(mode->pixclock);

	if (mode->vmode & (FB_VMODE_Y420 | FB_VMODE_Y420_ONLY))
		tmds_csc_8bpc_khz /= 2;

	return tmds_csc_8bpc_khz;
}

__maybe_unused
static bool tegra_hdmi_fb_mode_filter(const struct tegra_dc *dc,
					struct fb_videomode *mode)
{
	struct tegra_hdmi *hdmi = dc->out_data;

	if (!mode->pixclock)
		return false;

	if (mode->xres > 4096)
		return false;

#if defined(CONFIG_TEGRA_YUV_BYPASS_MODE_FILTER)
#if !defined(CONFIG_TEGRA_NVDISPLAY)
	/* No support for YUV modes on T210 hardware. Filter them out */
	if (mode->vmode & FB_VMODE_YUV_MASK)
		return false;
#else
	/* T186 hardware supports only YUV422. Filter out YUV420 modes */
	if ((mode->vmode & FB_VMODE_Y420_ONLY) ||
		(mode->vmode & FB_VMODE_Y420))
		return false;
#endif
#endif

	/* some non-compliant edids list 420vdb modes in vdb */
	if ((mode->vmode & FB_VMODE_Y420) &&
		!(tegra_edid_is_hfvsdb_present(hdmi->edid) &&
		tegra_edid_is_scdc_present(hdmi->edid)) &&
		tegra_edid_is_420db_present(hdmi->edid)) {
		mode->vmode &= ~FB_VMODE_Y420;
		mode->vmode |= FB_VMODE_Y420_ONLY;
	}

	if (mode->vmode & FB_VMODE_YUV_MASK &&
		tegra_edid_get_quirks(hdmi->edid) & TEGRA_EDID_QUIRK_NO_YUV)
		return false;

	/*
	 * There are currently many TVs in the market that actually do NOT support
	 * 4k@60fps 4:4:4 (594 MHz), (especially on the HDCP 2.2 ports), but
	 * advertise it in the DTD block in their EDIDs. The workaround for this port
	 * is to disable the 594 MHz mode if no HF-VSDB is present or if no SCDC
	 * support is indicated
	 */
	if (tegra_hdmi_mode_min_tmds_rate(mode) / 1000 >= 340 &&
		(!tegra_edid_is_hfvsdb_present(hdmi->edid) ||
		!tegra_edid_is_scdc_present(hdmi->edid)))
		return false;

	/*
	 * Check if mode's pixel clock requirement can be satisfied. Note that
	 * the pixclock value is in pico seconds.
	 */
	if (mode->pixclock && tegra_dc_get_out_max_pixclock(dc) &&
		mode->pixclock < tegra_dc_get_out_max_pixclock(dc))
		return false;

	/*
	 * Work around for modes that fail the constraint:
	 * V_FRONT_PORCH >= V_REF_TO_SYNC + 1
	 */
	if (mode->lower_margin == 1) {
		mode->lower_margin++;
		mode->upper_margin--;
		mode->vmode |= FB_VMODE_ADJUSTED;
	}

	if (!check_fb_videomode_timings(dc, mode)) {
		trace_printk("check_fb_videomode_timings: false\n"
			     "%u x %u @ %u Hz\n",
			     mode->xres, mode->yres, mode->pixclock);
		return false;
	}

	return true;
}

static void tegra_hdmi_ddc_power_toggle(int value)
{
	int partition_id;
	if (dc_hdmi == NULL)
		return;

#ifdef CONFIG_PM_GENERIC_DOMAINS_OF
	partition_id = tegra_pd_get_powergate_id(tegra_sor_pd);
	if (partition_id < 0)
		return;
#else
	partition_id = TEGRA_POWERGATE_SOR;
#endif

	if (value == 0) {
		_tegra_hdmi_ddc_disable(dc_hdmi);
		tegra_powergate_partition(partition_id);
	} else if (value == 1) {
		tegra_unpowergate_partition(partition_id);
		_tegra_hdmi_ddc_enable(dc_hdmi);
	}

	return;
}

static int tegra_hdmi_get_mon_spec(struct tegra_hdmi *hdmi)
{
#define MAX_RETRY 100
#define MIN_RETRY_DELAY_US 200
#define MAX_RETRY_DELAY_US (MIN_RETRY_DELAY_US + 200)

	size_t attempt_cnt = 0;
	int err = 0;
	struct i2c_adapter *i2c_adap = i2c_get_adapter(hdmi->dc->out->ddc_bus);

	if (IS_ERR_OR_NULL(hdmi->edid)) {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: edid not initialized\n");
		return PTR_ERR(hdmi->edid);
	}

	tegra_edid_i2c_adap_change_rate(i2c_adap, hdmi->ddc_i2c_original_rate);

	hdmi->mon_spec_valid = false;
	if (hdmi->mon_spec_valid)
		fb_destroy_modedb(hdmi->mon_spec.modedb);
	memset(&hdmi->mon_spec, 0, sizeof(hdmi->mon_spec));

	do {
		err = tegra_edid_get_monspecs(hdmi->edid, &hdmi->mon_spec);
		if (err < 0)
			usleep_range(MIN_RETRY_DELAY_US, MAX_RETRY_DELAY_US);
		else
			break;
	} while (++attempt_cnt < MAX_RETRY);

	if (err < 0) {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: edid read failed\n");
		/* Try to load and parse the fallback edid */
		hdmi->edid->errors = EDID_ERRORS_READ_FAILED;
		err = tegra_edid_get_monspecs(hdmi->edid, &hdmi->mon_spec);
		if (err < 0) {
			dev_err(&hdmi->dc->ndev->dev,
				"hdmi: parsing fallback edid failed\n");
			return err;
		} else {
			dev_err(&hdmi->dc->ndev->dev,
				"hdmi: using fallback edid\n");
		}
	}

	hdmi->mon_spec_valid = true;
	return 0;

#undef MAX_RETRY_DELAY_US
#undef MIN_RETRY_DELAY_US
#undef MAX_RETRY
}

static inline int tegra_hdmi_edid_read(struct tegra_hdmi *hdmi)
{
	int err;

	err = tegra_hdmi_get_mon_spec(hdmi);

	return err;
}

static int tegra_hdmi_get_eld(struct tegra_hdmi *hdmi)
{
	int err;

	hdmi->eld_valid = false;
	memset(&hdmi->eld, 0, sizeof(hdmi->eld));

	err = tegra_edid_get_eld(hdmi->edid, &hdmi->eld);
	if (err < 0) {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: eld not available\n");
		return err;
	}

	hdmi->eld_valid = true;
	return 0;
}

static inline int tegra_hdmi_eld_read(struct tegra_hdmi *hdmi)
{
	return tegra_hdmi_get_eld(hdmi);
}

static void tegra_hdmi_edid_config(struct tegra_hdmi *hdmi)
{
#define CM_TO_MM(x) (x * 10)

	struct tegra_dc *dc = hdmi->dc;

	if (!hdmi->mon_spec_valid)
		return;

	dc->out->h_size = CM_TO_MM(hdmi->mon_spec.max_x);
	dc->out->v_size = CM_TO_MM(hdmi->mon_spec.max_y);

	hdmi->dvi = !tegra_hdmi_is_connected(hdmi);
#undef CM_TO_MM
}

static void tegra_hdmi_hotplug_notify(struct tegra_hdmi *hdmi,
					bool is_asserted)
{
	struct tegra_dc *dc = hdmi->dc;
	struct fb_monspecs *mon_spec;

	if (is_asserted)
		mon_spec = &hdmi->mon_spec;
	else
		mon_spec = NULL;

#ifdef CONFIG_ADF_TEGRA
	if (dc->adf)
		tegra_adf_process_hotplug_connected(hdmi->dc->adf, mon_spec);
#else
	if (dc->fb) {
		tegra_fb_update_monspecs(hdmi->dc->fb, mon_spec,
					tegra_hdmi_fb_mode_filter);
		tegra_fb_update_fix(hdmi->dc->fb, mon_spec);
	}
#endif

	dc->connected = is_asserted;
	tegra_dc_ext_process_hotplug(dc->ndev->id, is_asserted);

#ifdef CONFIG_SWITCH
	switch_set_state(&hdmi->hpd_switch, is_asserted ? 1 : 0);
#endif
}

static int tegra_hdmi_edid_eld_setup(struct tegra_hdmi *hdmi)
{
	int err;

	tegra_dc_unpowergate_locked(hdmi->dc);

	err = tegra_hdmi_edid_read(hdmi);
	if (err < 0)
		goto fail;

	err = tegra_hdmi_eld_read(hdmi);
	if (err < 0)
		goto fail;

	err = tegra_hdmivrr_setup(hdmi);
	if (err < 0)
		dev_info(&hdmi->dc->ndev->dev, "vrr_setup failed\n");

	tegra_dc_powergate_locked(hdmi->dc);

	tegra_hdmi_edid_config(hdmi);

	/*
	 * eld is configured when audio needs it
	 * via tegra_hdmi_edid_config()
	 */

	tegra_hdmi_hotplug_notify(hdmi, true);
	return 0;
fail:
	tegra_dc_powergate_locked(hdmi->dc);
	return err;
}

static int tegra_hdmi_controller_disable(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	struct tegra_dc *dc = hdmi->dc;

	tegra_dc_get(dc);

	cancel_delayed_work_sync(&hdmi->scdc_work);
	if (dc->mode.pclk > 340000000) {
		tegra_hdmi_v2_x_mon_config(hdmi, false);
		tegra_hdmi_v2_x_host_config(hdmi, false);
	}

	tegra_nvhdcp_set_plug(hdmi->nvhdcp, 0);
	tegra_dc_sor_detach(sor);
	tegra_sor_power_lanes(sor, 4, false);
	tegra_sor_hdmi_pad_power_down(sor);
	tegra_hdmi_reset(hdmi);
	tegra_hdmi_put(dc);
	cancel_delayed_work_sync(&hdmi->hdr_worker);
	tegra_dc_put(dc);

	return 0;
}

static int tegra_hdmi_disable(struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;

	if (!hdmi->enabled) {
		dc->connected = false;
		tegra_dc_ext_process_hotplug(dc->ndev->id, false);
#ifdef CONFIG_SWITCH
		switch_set_state(&hdmi->hpd_switch, 0);
#endif
		return 0;
	}

	hdmi->enabled = false;
	hdmi->eld_valid = false;
	hdmi->mon_spec_valid = false;

	tegra_dc_disable(hdmi->dc);

	tegra_hdmi_hotplug_notify(hdmi, false);

	return 0;
}

static int (*tegra_hdmi_state_func[])(struct tegra_hdmi *) = {
	tegra_hdmi_disable,
	tegra_hdmi_edid_eld_setup,
};

enum tegra_hdmi_plug_states {
	TEGRA_HDMI_MONITOR_DISABLE,
	TEGRA_HDMI_MONITOR_ENABLE,
};

static int read_edid_into_buffer(struct tegra_hdmi *hdmi,
				 u8 *edid_data, size_t edid_data_len)
{
	int err, i;
	int extension_blocks;
	int max_ext_blocks = (edid_data_len / 128) - 1;

	err = tegra_edid_read_block(hdmi->edid, 0, edid_data);
	if (err) {
		dev_info(&hdmi->dc->ndev->dev, "hdmi: tegra_edid_read_block(0) returned err %d\n",
			err);
		return err;
	}
	extension_blocks = edid_data[0x7e];
	dev_info(&hdmi->dc->ndev->dev, "%s: extension_blocks = %d, max_ext_blocks = %d\n",
		__func__, extension_blocks, max_ext_blocks);
	if (extension_blocks > max_ext_blocks)
		extension_blocks = max_ext_blocks;
	for (i = 1; i <= extension_blocks; i++) {
		err = tegra_edid_read_block(hdmi->edid, i, edid_data + i * 128);
		if (err) {
			dev_info(&hdmi->dc->ndev->dev, "hdmi: tegra_edid_read_block(%d) returned err %d\n",
				i, err);
			return err;
		}
	}
	return i * 128;
}

static int hdmi_recheck_edid(struct tegra_hdmi *hdmi, int *match)
{
	int ret;
	u8 tmp[HDMI_EDID_MAX_LENGTH] = {0};
	ret = read_edid_into_buffer(hdmi, tmp, sizeof(tmp));
	dev_info(&hdmi->dc->ndev->dev, "%s: read_edid_into_buffer() returned %d\n",
		__func__, ret);
	if (ret > 0) {
		struct tegra_dc_edid *data = tegra_edid_get_data(hdmi->edid);
		dev_info(&hdmi->dc->ndev->dev, "old edid len = %ld\n",
			(long int)data->len);
		*match = ((ret == data->len) &&
			  !memcmp(tmp, data->buf, data->len));
		if (*match == 0) {
			print_hex_dump(KERN_INFO, "tmp :", DUMP_PREFIX_ADDRESS,
				       16, 4, tmp, ret, true);
			print_hex_dump(KERN_INFO, "data:", DUMP_PREFIX_ADDRESS,
				       16, 4, data->buf, data->len, true);
		}
		tegra_edid_put_data(data);
		ret = 0;
	}

	return ret;
}
static void tegra_hdmi_hpd_worker(struct work_struct *work)
{
	struct tegra_hdmi *hdmi = container_of(to_delayed_work(work),
				struct tegra_hdmi, hpd_worker);
	int err;
	bool connected;
	enum tegra_hdmi_plug_states orig_state;
	int match = 0;

	mutex_lock(&hdmi->hpd_lock);

	connected = tegra_dc_hpd(hdmi->dc);
	orig_state = hdmi->plug_state;

	if (connected) {
		switch (orig_state) {
		case TEGRA_HDMI_MONITOR_ENABLE:
			if (hdmi_recheck_edid(hdmi, &match)) {
				dev_info(&hdmi->dc->ndev->dev, "hdmi: unable to read EDID\n");
				goto fail;
			} else {
				if (match) {
					dev_info(&hdmi->dc->ndev->dev, "hdmi: No EDID change after HPD bounce, taking no action.\n");
					goto fail;
				} else {
					dev_info(&hdmi->dc->ndev->dev, "hdmi: EDID change after HPD bounce, resetting\n");
					if (hdmi->dc->out->hotplug_state !=
						TEGRA_HPD_STATE_FORCE_ASSERT)
						hdmi->plug_state = TEGRA_HDMI_MONITOR_DISABLE;
				}
			}
			break;
		case TEGRA_HDMI_MONITOR_DISABLE:
			hdmi->plug_state = TEGRA_HDMI_MONITOR_ENABLE;
			break;
		default:
			break;
		};
	} else {
		switch (orig_state) {
		case TEGRA_HDMI_MONITOR_ENABLE:
			hdmi->plug_state = TEGRA_HDMI_MONITOR_DISABLE;
			break;
		case TEGRA_HDMI_MONITOR_DISABLE:
			goto fail;
		default:
			break;
		};
	}

	err = tegra_hdmi_state_func[hdmi->plug_state](hdmi);

	if (err < 0) {
			dev_info(&hdmi->dc->ndev->dev,
				"hdmi state %d failed during %splug\n",
				hdmi->plug_state, connected ? "" : "un");
			hdmi->plug_state = orig_state;
			goto fail;
		} else {
			dev_info(&hdmi->dc->ndev->dev, "hdmi: %splugged\n",
					connected ? "" : "un");
		}

	if (connected && hdmi->plug_state == TEGRA_HDMI_MONITOR_DISABLE)
		goto reschedule_worker;

fail:
	mutex_unlock(&hdmi->hpd_lock);
	return;

reschedule_worker:
	mutex_unlock(&hdmi->hpd_lock);
	cancel_delayed_work(&hdmi->hpd_worker);
	schedule_delayed_work(&hdmi->hpd_worker, 0);
	return;

}

static irqreturn_t tegra_hdmi_hpd_irq_handler(int irq, void *ptr)
{
	struct tegra_dc *dc = ptr;
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	if (atomic_read(&hdmi->suspended))
		return IRQ_HANDLED;

	cancel_delayed_work(&hdmi->hpd_worker);
	schedule_delayed_work(&hdmi->hpd_worker,
				msecs_to_jiffies(HDMI_HPD_DEBOUNCE_DELAY_MS));

	return IRQ_HANDLED;
}

static int tegra_hdmi_hpd_init(struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;
	int hotplug_gpio = dc->out->hotplug_gpio;
	int hotplug_irq;
	int err;

	if (!gpio_is_valid(hotplug_gpio)) {
		dev_err(&dc->ndev->dev, "hdmi: invalid hotplug gpio\n");
		return -EINVAL;
	}

	hotplug_irq = gpio_to_irq(hotplug_gpio);
	if (hotplug_irq < 0) {
		dev_err(&dc->ndev->dev,
			"hdmi: hotplug gpio to irq map failed\n");
		return -EINVAL;
	}

	err = gpio_request(hotplug_gpio, "hdmi2.0_hpd");
	if (err < 0)
		dev_err(&dc->ndev->dev,
			"hdmi: hpd gpio_request failed %d\n", err);
	gpio_direction_input(hotplug_gpio);

	err = request_threaded_irq(hotplug_irq,
				NULL, tegra_hdmi_hpd_irq_handler,
				(IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT),
				dev_name(&dc->ndev->dev), dc);
	if (err) {
		dev_err(&dc->ndev->dev,
			"hdmi: request_threaded_irq failed: %d\n", err);
		goto fail;
	}


	INIT_DELAYED_WORK(&hdmi->hpd_worker, tegra_hdmi_hpd_worker);

	mutex_init(&hdmi->hpd_lock);
	hdmi->irq = hotplug_irq;

	return 0;
fail:
	gpio_free(hotplug_gpio);
	return err;
}

static int tegra_hdmi_tmds_init(struct tegra_hdmi *hdmi)
{
	struct device_node *np_prod = of_find_node_by_path(TMDS_NODE);

	if (!np_prod) {
		dev_warn(&hdmi->dc->ndev->dev,
			"hdmi: find tmds prod node failed\n");
		return -EINVAL;
	}

	hdmi->prod_list =
		tegra_prod_init((const struct device_node *)np_prod);
	if (IS_ERR(hdmi->prod_list)) {
		dev_warn(&hdmi->dc->ndev->dev,
			"hdmi: prod list init failed with error %ld\n",
			PTR_ERR(hdmi->prod_list));
		of_node_put(np_prod);
		return -EINVAL;
	}

	of_node_put(np_prod);
	return 0;
}

static int tegra_hdmi_config_tmds(struct tegra_hdmi *hdmi)
{
	size_t tmds_len;
	int i;
	int err = 0;

	/* Select mode with smallest clk freq > pclk */
	tmds_len = ARRAY_SIZE(tmds_config_modes);
	for (i = 0; i < tmds_len - 1 &&
		tmds_config_modes[i].clk < hdmi->dc->mode.pclk; i++);

	if (tegra_platform_is_linsim())
		return 0;

	err = tegra_prod_set_by_name(&hdmi->sor->base,
				tmds_config_modes[i].name, hdmi->prod_list);
	if (err) {
		dev_warn(&hdmi->dc->ndev->dev,
			"hdmi: tmds prod set failed\n");
		return -EINVAL;
	}

	return 0;
}

static int tegra_hdmi_hdr_init(struct tegra_hdmi *hdmi)
{
	INIT_DELAYED_WORK(&hdmi->hdr_worker, tegra_hdmi_hdr_worker);
	return 0;
}

static int tegra_dc_hdmi_init(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi;
	int err;
	struct device_node *np = dc->ndev->dev.of_node;
#ifdef CONFIG_OF
	struct device_node *np_hdmi = of_find_node_by_path(HDMI_NODE);
#else
	struct device_node *np_hdmi = NULL;
#endif
	struct device_node *np_panel = NULL;
	int edid_src = 0;

	hdmi = devm_kzalloc(&dc->ndev->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi) {
		of_node_put(np_hdmi);
		return -ENOMEM;
	}

	hdmi->sor = tegra_dc_sor_init(dc, NULL);
	if (IS_ERR_OR_NULL(hdmi->sor)) {
		err = PTR_ERR(hdmi->sor);
		goto fail;
	}

	if (np) {
		if (np_hdmi && of_device_is_available(np_hdmi)) {
			np_panel = tegra_get_panel_node_out_type_check(dc,
				TEGRA_DC_OUT_HDMI);
			if (np_panel && of_device_is_available(np_panel)) {
				if (of_property_read_bool(np_panel,
							"nvidia,edid"))
					edid_src = 1;
				of_node_put(np_panel);
			}
		} else {
			err = -EINVAL;
			of_node_put(np_panel);
			goto fail;
		}
	}

	hdmi->pdata = dc->pdata->default_out->hdmi_out;
	hdmi->dc = dc;
	dc_hdmi = hdmi;
	hdmi->ddc_refcount = 0; /* assumes this is disabled when starting */
	mutex_init(&hdmi->ddc_refcount_lock);
	hdmi->nvhdcp = NULL;
	hdmi->mon_spec_valid = false;
	hdmi->eld_valid = false;
	hdmi->device_shutdown = false;
	if (0) {
		/* TODO: seamless boot mode needs initialize the state */
	} else {
		hdmi->enabled = false;
		atomic_set(&hdmi->clock_refcount, 0);
	}
	atomic_set(&hdmi->suspended, 0);

#ifdef CONFIG_TEGRA_HDMIHDCP
	hdmi->nvhdcp = tegra_nvhdcp_create(hdmi, dc->ndev->id,
			dc->out->ddc_bus);
	if (IS_ERR_OR_NULL(hdmi->nvhdcp)) {
		err = PTR_ERR(hdmi->nvhdcp);
		goto fail;
	}
	tegra_nvhdcp_debugfs_init(hdmi->nvhdcp);
#endif

	tegra_hdmi_ddc_init(hdmi, edid_src);

	tegra_hdmi_scdc_init(hdmi);

	tegra_hdmi_hpd_init(hdmi);

	tegra_hdmi_vrr_init(hdmi);

	tegra_hdmi_debugfs_init(hdmi);

	tegra_hdmi_tmds_init(hdmi);

	tegra_dc_set_outdata(dc, hdmi);

	tegra_hdmi_hdr_init(hdmi);

	/* NOTE: Below code is applicable to L4T or embedded systems and is
	 * protected accordingly. This section early enables DC with first mode
	 * from the monitor specs.
	 * In case there is no hotplug we are falling back
	 * to default VGA mode.
	 */
	if ((config_enabled(CONFIG_FRAMEBUFFER_CONSOLE) ||
			((dc->pdata->flags & TEGRA_DC_FLAG_ENABLED) &&
			 (dc->pdata->flags & TEGRA_DC_FLAG_SET_EARLY_MODE))) &&
			dc->out && (dc->out->type == TEGRA_DC_OUT_HDMI)) {
		struct fb_monspecs specs;
		if (tegra_dc_hpd(dc) && (!dc->initialized)) {
			if (!tegra_edid_get_monspecs(hdmi->edid, &specs))
				tegra_dc_set_fb_mode(dc, specs.modedb, false);
			else {
			/* if for some reason there is no edid upon hotplug */
				tegra_dc_set_fb_mode(dc,
						&tegra_dc_vga_mode, false);
			}
		} else
			tegra_dc_set_fb_mode(dc, &tegra_dc_vga_mode, false);
	}

#ifdef CONFIG_SWITCH
	hdmi->hpd_switch.name = "hdmi";
	err = switch_dev_register(&hdmi->hpd_switch);
	if (err)
		dev_err(&dc->ndev->dev,
			"hdmi: failed to register hpd switch %d\n", err);

	hdmi->audio_switch.name = "hdmi_audio";
	err = switch_dev_register(&hdmi->audio_switch);
	if (err)
		dev_err(&dc->ndev->dev,
			"hdmi: failed to register audio switch %d\n", err);
#endif

	hdmi_ddc = kobject_create_and_add("hdmi_ddc_power_toggle", kernel_kobj);
	if (!hdmi_ddc) {
		pr_warn("kobject create_and_add hdmi_ddc_power_toggle failed\n");
		return 0;
	}
	err = sysfs_create_file(hdmi_ddc, &hdmi_ddc_power_config.attr);
	if (err) {
		pr_warn("sysfs create file hdmi_ddc_power_toggle failed\n");
		return 0;
	}

	of_node_put(np_hdmi);
	return 0;
fail:
	devm_kfree(&dc->ndev->dev, hdmi);
	of_node_put(np_hdmi);
	return err;
}

static void tegra_dc_hdmi_destroy(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	tegra_dc_sor_destroy(hdmi->sor);
	tegra_edid_destroy(hdmi->edid);
	tegra_nvhdcp_destroy(hdmi->nvhdcp);
	free_irq(gpio_to_irq(dc->out->hotplug_gpio), dc);
	gpio_free(dc->out->hotplug_gpio);
	devm_kfree(&dc->ndev->dev, hdmi);
	tegra_prod_release(&hdmi->prod_list);

#ifdef CONFIG_SWITCH
	switch_dev_unregister(&hdmi->hpd_switch);
	switch_dev_unregister(&hdmi->audio_switch);
#endif
}

static void tegra_hdmi_config(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	struct tegra_dc *dc = hdmi->dc;
#ifndef CONFIG_TEGRA_NVDISPLAY
	u32 h_pulse_start, h_pulse_end;
#endif
	u32 hblank, max_ac, rekey;
	unsigned long val;
	bool rgb_limited;
	u32 dispclk_div_8_2;

	if (tegra_platform_is_linsim())
		return;

	rgb_limited = (dc->mode.vmode & FB_VMODE_LIMITED_RANGE) &&
		      !(dc->mode.vmode & FB_VMODE_YUV_MASK);

	tegra_sor_write_field(sor, NV_SOR_INPUT_CONTROL,
			NV_SOR_INPUT_CONTROL_ARM_VIDEO_RANGE_LIMITED |
			NV_SOR_INPUT_CONTROL_HDMI_SRC_SELECT_DISPLAYB,
			(rgb_limited ?
			NV_SOR_INPUT_CONTROL_ARM_VIDEO_RANGE_LIMITED :
			NV_SOR_INPUT_CONTROL_ARM_VIDEO_RANGE_FULL) |
			NV_SOR_INPUT_CONTROL_HDMI_SRC_SELECT_DISPLAYB);

	dispclk_div_8_2 = clk_get_rate(hdmi->sor->sor_clk) / 1000000 * 4;
	tegra_sor_writel(sor, NV_SOR_REFCLK,
			NV_SOR_REFCLK_DIV_INT(dispclk_div_8_2 >> 2) |
			NV_SOR_REFCLK_DIV_FRAC(dispclk_div_8_2));

	/*
	 * The rekey register and corresponding eq want to operate
	 * on "-2" of the desired rekey value
	 */
	rekey = NV_SOR_HDMI_CTRL_REKEY_DEFAULT - 2;
	hblank = dc->mode.h_sync_width + dc->mode.h_back_porch +
			dc->mode.h_front_porch;
	max_ac = (hblank - rekey - 18) / 32;

	val = 0;
	val |= hdmi->dvi ? 0x0 : NV_SOR_HDMI_CTRL_ENABLE;
	/* The register wants "-2" of the required rekey val */
	val |= NV_SOR_HDMI_CTRL_REKEY(rekey);
	val |= NV_SOR_HDMI_CTRL_MAX_AC_PACKET(max_ac);
	val |= NV_SOR_HDMI_CTRL_AUDIO_LAYOUT_SELECT;
	tegra_sor_writel(sor, NV_SOR_HDMI_CTRL, val);

#ifndef CONFIG_TEGRA_NVDISPLAY
	tegra_dc_writel(dc, 0x180, DC_DISP_H_PULSE2_CONTROL);
	h_pulse_start = dc->mode.h_ref_to_sync +
					dc->mode.h_sync_width +
					dc->mode.h_back_porch - 10;
	h_pulse_end = h_pulse_start + 8;
	tegra_dc_writel(dc, PULSE_START(h_pulse_start) | PULSE_END(h_pulse_end),
		  DC_DISP_H_PULSE2_POSITION_A);

	val = tegra_dc_readl(dc, DC_DISP_DISP_SIGNAL_OPTIONS0);
	val |= H_PULSE_2_ENABLE;
	tegra_dc_writel(dc, val, DC_DISP_DISP_SIGNAL_OPTIONS0);
#endif
}

void tegra_hdmi_infoframe_pkt_write(struct tegra_hdmi *hdmi,
						u32 header_reg, u8 pkt_type,
						u8 pkt_vs, u8 pkt_len,
						void *reg_payload,
						u32 reg_payload_len,
						bool sw_checksum)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	u32 val;
	u32 *data = reg_payload;
	u32 data_reg = header_reg + 1;

	val = NV_SOR_HDMI_INFOFRAME_HEADER_TYPE(pkt_type) |
		NV_SOR_HDMI_INFOFRAME_HEADER_VERSION(pkt_vs) |
		NV_SOR_HDMI_INFOFRAME_HEADER_LEN(pkt_len);
	tegra_sor_writel(sor, header_reg, val);

	if (sw_checksum) {
		u8 checksum = pkt_type + pkt_vs + pkt_len;

		for (val = 1; val <= pkt_len; val++)
			checksum += ((u8 *)reg_payload)[val];

		/* The first byte of the payload must always be the checksum
		 * that we are going to calculate in SW */
		((u8 *)reg_payload)[0] = (256 - checksum);
	}

	for (val = 0; val < reg_payload_len; val += 4, data_reg++, data++)
		tegra_sor_writel(sor, data_reg, *data);
}

u32 tegra_hdmi_get_cea_modedb_size(struct tegra_hdmi *hdmi)
{
	if (!tegra_hdmi_is_connected(hdmi) || !hdmi->mon_spec_valid)
		return 0;

	return (hdmi->mon_spec.misc & FB_MISC_HDMI_FORUM) ?
		CEA_861_F_MODEDB_SIZE : CEA_861_D_MODEDB_SIZE;
}

static void tegra_hdmi_get_cea_fb_videomode(struct fb_videomode *m,
						struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;
	struct tegra_dc_mode dc_mode;
	int yuv_flag;

	memcpy(&dc_mode, &dc->mode, sizeof(dc->mode));

	/* get CEA video timings */
	yuv_flag = dc_mode.vmode & FB_VMODE_YUV_MASK;
	if (yuv_flag == (FB_VMODE_Y420      | FB_VMODE_Y24) ||
	    yuv_flag == (FB_VMODE_Y420_ONLY | FB_VMODE_Y24)) {
		dc_mode.h_back_porch *= 2;
		dc_mode.h_front_porch *= 2;
		dc_mode.h_sync_width *= 2;
		dc_mode.h_active *= 2;
		dc_mode.pclk *= 2;
	} else if (yuv_flag == (FB_VMODE_Y420 | FB_VMODE_Y30)) {
		dc_mode.h_back_porch = (dc_mode.h_back_porch * 8) / 5;
		dc_mode.h_front_porch = (dc_mode.h_front_porch * 8) / 5;
		dc_mode.h_sync_width = (dc_mode.h_sync_width * 8) / 5;
		dc_mode.h_active = (dc_mode.h_active * 8) / 5;
		dc_mode.pclk = (dc_mode.pclk / 5) * 8;
	}

	tegra_dc_to_fb_videomode(m, &dc_mode);

	/* only interlaced required for VIC identification */
	m->vmode &= FB_VMODE_INTERLACED;
}

__maybe_unused
static int tegra_hdmi_find_cea_vic(struct tegra_hdmi *hdmi)
{
	struct fb_videomode m;
	struct tegra_dc *dc = hdmi->dc;
	unsigned i;
	unsigned best = 0;
	u32 modedb_size = tegra_hdmi_get_cea_modedb_size(hdmi);

	if (dc->initialized) {
		u32 vic = tegra_sor_readl(hdmi->sor,
			NV_SOR_HDMI_AVI_INFOFRAME_SUBPACK0_HIGH) & 0xff;
		if (!vic)
			dev_warn(&dc->ndev->dev, "hdmi: BL set VIC 0\n");
		return vic;
	}

	tegra_hdmi_get_cea_fb_videomode(&m, hdmi);

	m.flag &= ~FB_FLAG_RATIO_MASK;
	m.flag |= tegra_dc_get_aspect_ratio(dc);

	for (i = 1; i < modedb_size; i++) {
		const struct fb_videomode *curr = &cea_modes[i];

		if (!fb_mode_is_equal_tolerance(curr, &m,
			FB_MODE_TOLERANCE_DEFAULT))
			continue;

		if (!best)
			best = i;
		/* if either flag is set, then match is required */
		if (curr->flag &
			(FB_FLAG_RATIO_4_3 | FB_FLAG_RATIO_16_9 |
			FB_FLAG_RATIO_64_27 | FB_FLAG_RATIO_256_135)) {
			if (m.flag & curr->flag & FB_FLAG_RATIO_4_3)
				best = i;
			else if (m.flag & curr->flag & FB_FLAG_RATIO_16_9)
				best = i;
			else if (m.flag & curr->flag & FB_FLAG_RATIO_64_27)
				best = i;
			else if (m.flag & curr->flag & FB_FLAG_RATIO_256_135)
				best = i;
		} else {
			best = i;
		}
	}
	return best;
}

static u32 tegra_hdmi_get_aspect_ratio(struct tegra_hdmi *hdmi)
{
	u32 aspect_ratio;

	switch (hdmi->dc->mode.avi_m) {
	case TEGRA_DC_MODE_AVI_M_4_3:
		aspect_ratio = HDMI_AVI_ASPECT_RATIO_4_3;
		break;
	case TEGRA_DC_MODE_AVI_M_16_9:
		aspect_ratio = HDMI_AVI_ASPECT_RATIO_16_9;
		break;
	/*
	 * no avi_m field for picture aspect ratio 64:27 and 256:135.
	 * sink detects via VIC, avi_m is 0.
	 */
	case TEGRA_DC_MODE_AVI_M_64_27: /* fall through */
	case TEGRA_DC_MODE_AVI_M_256_135: /* fall through */
	default:
		aspect_ratio = HDMI_AVI_ASPECT_RATIO_NO_DATA;
	}

	/* For seamless HDMI, read aspect ratio parameters from bootloader
	 * set AVI Infoframe parameters
	 */
	if ((aspect_ratio == HDMI_AVI_ASPECT_RATIO_NO_DATA) &&
					(hdmi->dc->initialized)) {
		u32 temp = 0;
		temp = tegra_sor_readl(hdmi->sor,
			NV_SOR_HDMI_AVI_INFOFRAME_SUBPACK0_LOW);
		temp = (temp >> 20) & 0x3;
		switch (temp) {
		case 1:
		aspect_ratio = HDMI_AVI_ASPECT_RATIO_4_3;
		break;
		case 2:
		aspect_ratio = HDMI_AVI_ASPECT_RATIO_16_9;
		break;
		default:
		aspect_ratio = HDMI_AVI_ASPECT_RATIO_NO_DATA;
		}
	}
	return aspect_ratio;
}

static u32 tegra_hdmi_get_rgb_ycc(struct tegra_hdmi *hdmi)
{
	int yuv_flag = hdmi->dc->mode.vmode & FB_VMODE_YUV_MASK;

	/*
	 * For seamless HDMI, read YUV flag parameters from bootloader
	 * set AVI Infoframe parameters
	 */
	if (hdmi->dc->initialized) {
		u32 temp = 0;
		temp = tegra_sor_readl(hdmi->sor,
			NV_SOR_HDMI_AVI_INFOFRAME_SUBPACK0_LOW);
		temp = (temp >> 12) & 0x3;
		switch (temp) {
		case HDMI_AVI_RGB:
			return HDMI_AVI_RGB;
		case HDMI_AVI_YCC_420:
			return HDMI_AVI_YCC_420;
		default:
			dev_warn(&hdmi->dc->ndev->dev, "hdmi: BL didn't set RGB/YUV indicator flag\n");
			break;
		}
	}

	if (yuv_flag & (FB_VMODE_Y420 | FB_VMODE_Y420_ONLY))
		return HDMI_AVI_YCC_420;
	else if (yuv_flag & FB_VMODE_Y422)
		return HDMI_AVI_YCC_422;
	else if (yuv_flag & FB_VMODE_Y444)
		return HDMI_AVI_YCC_444;

	return HDMI_AVI_RGB;
}

static bool tegra_hdmi_is_ex_colorimetry(struct tegra_hdmi *hdmi)
{
	return !!(hdmi->dc->mode.vmode & FB_VMODE_EC_ENABLE);
}

static u32 tegra_hdmi_get_ex_colorimetry(struct tegra_hdmi *hdmi)
{
	u32 vmode = hdmi->dc->mode.vmode;

	return tegra_hdmi_is_ex_colorimetry(hdmi) ?
		((vmode & FB_VMODE_EC_MASK) >> FB_VMODE_EC_SHIFT) :
		HDMI_AVI_EXT_COLORIMETRY_INVALID;
}

static u32 tegra_hdmi_get_rgb_quant(struct tegra_hdmi *hdmi)
{
	u32 vmode = hdmi->dc->mode.vmode;

	if (tegra_edid_get_quant_cap(hdmi->edid) & FB_CAP_RGB_QUANT_SELECTABLE)
		return vmode & FB_VMODE_LIMITED_RANGE ?
			HDMI_AVI_RGB_QUANT_LIMITED : HDMI_AVI_RGB_QUANT_FULL;
	else
		/*
		 * The safest way to break the HDMI spec when forcing full range
		 * on a limited system: send full data with the QUANT_DEFAULT
		 * */
		return HDMI_AVI_RGB_QUANT_DEFAULT;
}

static u32 tegra_hdmi_get_ycc_quant(struct tegra_hdmi *hdmi)
{
	u32 vmode = hdmi->dc->mode.vmode;

	if (tegra_edid_get_quant_cap(hdmi->edid) & FB_CAP_YUV_QUANT_SELECTABLE)
		return vmode & FB_VMODE_LIMITED_RANGE ?
			HDMI_AVI_YCC_QUANT_LIMITED : HDMI_AVI_YCC_QUANT_FULL;
	else
		/*
		 * The safest way to break the HDMI spec when forcing full range
		 * on a limited system: send full data with the QUANT_DEFAULT
		 * */
		return HDMI_AVI_YCC_QUANT_NONE;
}

static void tegra_hdmi_avi_infoframe_update(struct tegra_hdmi *hdmi)
{
	struct hdmi_avi_infoframe *avi = &hdmi->avi;

	memset(&hdmi->avi, 0, sizeof(hdmi->avi));

	if (tegra_platform_is_linsim())
		return;

	avi->scan = HDMI_AVI_UNDERSCAN;
	avi->bar_valid = HDMI_AVI_BAR_INVALID;
	avi->act_fmt_valid = HDMI_AVI_ACTIVE_FORMAT_INVALID;
	avi->rgb_ycc = tegra_hdmi_get_rgb_ycc(hdmi);

	avi->act_format = HDMI_AVI_ACTIVE_FORMAT_SAME;
	avi->aspect_ratio = tegra_hdmi_get_aspect_ratio(hdmi);
	avi->colorimetry = tegra_hdmi_is_ex_colorimetry(hdmi) ?
			HDMI_AVI_COLORIMETRY_EXTENDED_VALID :
			HDMI_AVI_COLORIMETRY_DEFAULT;

	avi->scaling = HDMI_AVI_SCALING_UNKNOWN;
	avi->rgb_quant = tegra_hdmi_get_rgb_quant(hdmi);
	avi->ext_colorimetry = tegra_hdmi_get_ex_colorimetry(hdmi);
	avi->it_content = HDMI_AVI_IT_CONTENT_FALSE;

	/* set correct vic if video format is cea defined else set 0 */
	avi->video_format = tegra_hdmi_find_cea_vic(hdmi);

	avi->pix_rep = HDMI_AVI_NO_PIX_REPEAT;
	avi->it_content_type = HDMI_AVI_IT_CONTENT_NONE;
	avi->ycc_quant = tegra_hdmi_get_ycc_quant(hdmi);

	avi->top_bar_end_line_low_byte = 0;
	avi->top_bar_end_line_high_byte = 0;

	avi->bot_bar_start_line_low_byte = 0;
	avi->bot_bar_start_line_high_byte = 0;

	avi->left_bar_end_pixel_low_byte = 0;
	avi->left_bar_end_pixel_high_byte = 0;

	avi->right_bar_start_pixel_low_byte = 0;
	avi->right_bar_start_pixel_high_byte = 0;
}

static void tegra_hdmi_avi_infoframe(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;

	if (hdmi->dvi)
		return;

	if (tegra_platform_is_linsim())
		return;

	/* disable avi infoframe before configuring except for seamless case */
	if (!hdmi->dc->initialized)
		tegra_sor_writel(sor, NV_SOR_HDMI_AVI_INFOFRAME_CTRL, 0);

	tegra_hdmi_avi_infoframe_update(hdmi);

	tegra_hdmi_infoframe_pkt_write(hdmi, NV_SOR_HDMI_AVI_INFOFRAME_HEADER,
					HDMI_INFOFRAME_TYPE_AVI,
					HDMI_INFOFRAME_VS_AVI,
					HDMI_INFOFRAME_LEN_AVI,
					&hdmi->avi, sizeof(hdmi->avi),
					false);

	/* Send infoframe every frame, checksum hw generated */
	tegra_sor_writel(sor, NV_SOR_HDMI_AVI_INFOFRAME_CTRL,
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_ENABLE_YES |
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_OTHER_DISABLE |
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_SINGLE_DISABLE |
		NV_SOR_HDMI_AVI_INFOFRAME_CTRL_CHECKSUM_ENABLE);
}

static int tegra_hdmi_get_extended_vic(const struct tegra_dc_mode *mode)
{
	struct fb_videomode m;
	unsigned i;

	tegra_dc_to_fb_videomode(&m, mode);

	/* only interlaced required for VIC identification */
	m.vmode &= FB_VMODE_INTERLACED;

	for (i = 1; i < HDMI_EXT_MODEDB_SIZE; i++) {
		const struct fb_videomode *curr = &hdmi_ext_modes[i];

		if (fb_mode_is_equal_tolerance(curr, &m,
			FB_MODE_TOLERANCE_DEFAULT))
			return i;
	}
	return 0;
}

static void tegra_hdmi_vendor_infoframe_update(struct tegra_hdmi *hdmi)
{
	struct hdmi_vendor_infoframe *vsi = &hdmi->vsi;
	u8 extended_vic;

	memset(&hdmi->vsi, 0, sizeof(hdmi->vsi));

	vsi->oui = HDMI_LICENSING_LLC_OUI;

	extended_vic = tegra_hdmi_get_extended_vic(&hdmi->dc->mode);
	if (extended_vic) {
		vsi->video_format =
			HDMI_VENDOR_VIDEO_FORMAT_EXTENDED;
		vsi->extended_vic = extended_vic;
	}
}

static void tegra_hdmi_vendor_infoframe(struct tegra_hdmi *hdmi)
{
/* hdmi licensing, LLC vsi playload len as per hdmi1.4b  */
#define HDMI_INFOFRAME_LEN_VENDOR_LLC	(6)

	struct tegra_dc_sor_data *sor = hdmi->sor;

	if (hdmi->dvi)
		return;

	/* disable vsi infoframe before configuring */
	tegra_sor_writel(sor, NV_SOR_HDMI_VSI_INFOFRAME_CTRL, 0);

	tegra_hdmi_vendor_infoframe_update(hdmi);

	tegra_hdmi_infoframe_pkt_write(hdmi, NV_SOR_HDMI_VSI_INFOFRAME_HEADER,
					HDMI_INFOFRAME_TYPE_VENDOR,
					HDMI_INFOFRAME_VS_VENDOR,
					HDMI_INFOFRAME_LEN_VENDOR_LLC,
					&hdmi->vsi, sizeof(hdmi->vsi),
					false);

	/* Send infoframe every frame, checksum hw generated */
	tegra_sor_writel(sor, NV_SOR_HDMI_VSI_INFOFRAME_CTRL,
		NV_SOR_HDMI_VSI_INFOFRAME_CTRL_ENABLE_YES |
		NV_SOR_HDMI_VSI_INFOFRAME_CTRL_OTHER_DISABLE |
		NV_SOR_HDMI_VSI_INFOFRAME_CTRL_SINGLE_DISABLE |
		NV_SOR_HDMI_VSI_INFOFRAME_CTRL_CHECKSUM_ENABLE);

#undef HDMI_INFOFRAME_LEN_VENDOR_LLC
}

static void tegra_hdmi_hdr_infoframe_update(struct tegra_hdmi *hdmi)
{
	struct hdmi_hdr_infoframe *hdr = &hdmi->hdr;

	memset(&hdmi->hdr, 0, sizeof(hdmi->hdr));

	if (tegra_platform_is_linsim())
		return;

	hdr->eotf = hdmi->dc->hdr.eotf;
	hdr->static_metadata_id = hdmi->dc->hdr.static_metadata_id;

	/* PB3-14 : Group 1 : Static Metadata*/
	hdr->display_primaries_x_0_lsb = hdmi->dc->hdr.static_metadata[0];
	hdr->display_primaries_x_0_msb = hdmi->dc->hdr.static_metadata[1];
	hdr->display_primaries_y_0_lsb = hdmi->dc->hdr.static_metadata[2];
	hdr->display_primaries_y_0_msb = hdmi->dc->hdr.static_metadata[3];
	hdr->display_primaries_x_1_lsb = hdmi->dc->hdr.static_metadata[4];
	hdr->display_primaries_x_1_msb = hdmi->dc->hdr.static_metadata[5];
	hdr->display_primaries_y_1_lsb = hdmi->dc->hdr.static_metadata[6];
	hdr->display_primaries_y_1_msb = hdmi->dc->hdr.static_metadata[7];
	hdr->display_primaries_x_2_lsb = hdmi->dc->hdr.static_metadata[8];
	hdr->display_primaries_x_2_msb = hdmi->dc->hdr.static_metadata[9];
	hdr->display_primaries_y_2_lsb = hdmi->dc->hdr.static_metadata[10];
	hdr->display_primaries_y_2_msb = hdmi->dc->hdr.static_metadata[11];

	/* PB15-18 : Group 2 : Static Metadata*/
	hdr->white_point_x_lsb = hdmi->dc->hdr.static_metadata[12];
	hdr->white_point_x_msb = hdmi->dc->hdr.static_metadata[13];
	hdr->white_point_y_lsb = hdmi->dc->hdr.static_metadata[14];
	hdr->white_point_y_msb = hdmi->dc->hdr.static_metadata[15];

	/* PB19-20 : Group 3 : Static Metadata*/
	hdr->max_display_mastering_luminance_lsb =
					hdmi->dc->hdr.static_metadata[16];
	hdr->max_display_mastering_luminance_msb =
					hdmi->dc->hdr.static_metadata[17];

	/* PB21-22 : Group 4 : Static Metadata*/
	hdr->min_display_mastering_luminance_lsb =
					hdmi->dc->hdr.static_metadata[18];
	hdr->min_display_mastering_luminance_msb =
					hdmi->dc->hdr.static_metadata[19];

	/* PB23-24 : Group 5 : Static Metadata*/
	hdr->max_content_light_level_lsb = hdmi->dc->hdr.static_metadata[20];
	hdr->max_content_light_level_msb = hdmi->dc->hdr.static_metadata[21];

	/* PB25-26 : Group 6 : Static Metadata*/
	hdr->max_frame_avg_light_level_lsb = hdmi->dc->hdr.static_metadata[22];
	hdr->min_frame_avg_light_level_msb = hdmi->dc->hdr.static_metadata[23];

	return;
}

static void tegra_hdmi_hdr_infoframe(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;

	/* set_bits = contains all the bits to be set
	 * for NV_SOR_HDMI_GENERIC_CTRL reg */
	u32 set_bits = (NV_SOR_HDMI_GENERIC_CTRL_ENABLE_YES |
			NV_SOR_HDMI_GENERIC_CTRL_AUDIO_ENABLE);

	/* disable generic infoframe before configuring */
	tegra_sor_writel(sor, NV_SOR_HDMI_GENERIC_CTRL, 0);

	tegra_hdmi_hdr_infoframe_update(hdmi);

	tegra_hdmi_infoframe_pkt_write(hdmi, NV_SOR_HDMI_GENERIC_HEADER,
					HDMI_INFOFRAME_TYPE_HDR,
					HDMI_INFOFRAME_VS_HDR,
					HDMI_INFOFRAME_LEN_HDR,
					&hdmi->hdr, sizeof(hdmi->hdr),
					true);

	/* set the required bits in NV_SOR_HDMI_GENERIC_CTRL*/
	tegra_sor_writel(sor, NV_SOR_HDMI_GENERIC_CTRL, set_bits);

	return;
}

__maybe_unused
static int tegra_hdmi_scdc_read(struct tegra_hdmi *hdmi,
					u8 offset_data[][2], u32 n_entries)
{
	u32 i;
	struct i2c_msg msg[] = {
		{
			.addr = 0x54,
			.len = 1,
			.buf = NULL,
		},
		{
			.addr = 0x54,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = NULL,
		},
	};

	_tegra_hdmi_ddc_enable(hdmi);

	for (i = 0; i < n_entries; i++) {
		msg[0].buf = offset_data[i];
		msg[1].buf = &offset_data[i][1];
		tegra_hdmi_scdc_i2c_xfer(hdmi->dc, msg, ARRAY_SIZE(msg));
	}

	_tegra_hdmi_ddc_disable(hdmi);

	return 0;
}

static int tegra_hdmi_scdc_write(struct tegra_hdmi *hdmi,
					u8 offset_data[][2], u32 n_entries)
{
	u32 i;
	struct i2c_msg msg[] = {
		{
			.addr = 0x54,
			.len = 2,
			.buf = NULL,
		},
	};

	_tegra_hdmi_ddc_enable(hdmi);

	for (i = 0; i < n_entries; i++) {
		msg[0].buf = offset_data[i];
		tegra_hdmi_scdc_i2c_xfer(hdmi->dc, msg, ARRAY_SIZE(msg));
	}

	_tegra_hdmi_ddc_disable(hdmi);

	return 0;
}

static int tegra_hdmi_v2_x_mon_config(struct tegra_hdmi *hdmi, bool enable)
{
	u8 tmds_config_en[][2] = {
		{
			HDMI_SCDC_TMDS_CONFIG_OFFSET,
			(HDMI_SCDC_TMDS_CONFIG_BIT_CLK_RATIO_40 |
			HDMI_SCDC_TMDS_CONFIG_SCRAMBLING_EN)
		},
	};
	u8 tmds_config_dis[][2] = {
		{
			HDMI_SCDC_TMDS_CONFIG_OFFSET,
			0
		},
	};

	if (hdmi->dc->vedid)
		goto skip_scdc_i2c;

	tegra_hdmi_scdc_write(hdmi,
			enable ? tmds_config_en : tmds_config_dis,
			ARRAY_SIZE(tmds_config_en));

skip_scdc_i2c:
	return 0;
}

static void tegra_hdmi_v2_x_host_config(struct tegra_hdmi *hdmi, bool enable)
{
	u32 val = NV_SOR_HDMI2_CTRL_SCRAMBLE_ENABLE |
		NV_SOR_HDMI2_CTRL_CLK_MODE_DIV_BY_4;

	tegra_sor_write_field(hdmi->sor, NV_SOR_HDMI2_CTRL,
			NV_SOR_HDMI2_CTRL_SCRAMBLE_ENABLE |
			NV_SOR_HDMI2_CTRL_CLK_MODE_DIV_BY_4,
			enable ? val : 0);
}

static int _tegra_hdmi_v2_x_config(struct tegra_hdmi *hdmi)
{
#define SCDC_STABILIZATION_DELAY_MS (20)

	/* disable hdmi2.x config on host and monitor only
	 * if bootloader didn't initialize hdmi
	 */
	if (!hdmi->dc->initialized) {
		tegra_hdmi_v2_x_mon_config(hdmi, false);
		tegra_hdmi_v2_x_host_config(hdmi, false);
	}

	/* enable hdmi2.x config on host and monitor */
	tegra_hdmi_v2_x_mon_config(hdmi, true);
	msleep(SCDC_STABILIZATION_DELAY_MS);

	tegra_hdmi_v2_x_host_config(hdmi, true);

	return 0;
#undef SCDC_STABILIZATION_DELAY_MS
}

static int tegra_hdmi_v2_x_config(struct tegra_hdmi *hdmi)
{
	_tegra_hdmi_v2_x_config(hdmi);

	return 0;
}

static void tegra_hdmi_scdc_worker(struct work_struct *work)
{
	struct tegra_hdmi *hdmi = container_of(to_delayed_work(work),
				struct tegra_hdmi, scdc_work);
	u8 rd_status_flags[][2] = {
		{HDMI_SCDC_STATUS_FLAGS, 0x0}
	};

	if (!hdmi->enabled || hdmi->dc->mode.pclk <= 340000000)
		return;

	if (hdmi->dc->vedid)
		goto skip_scdc_i2c;

	if (!tegra_edid_is_scdc_present(hdmi->dc->edid))
		return;

	tegra_hdmi_scdc_read(hdmi, rd_status_flags,
					ARRAY_SIZE(rd_status_flags));
	if (!rd_status_flags[0][1]  && (hdmi->dc->mode.pclk > 340000000)) {
		dev_info(&hdmi->dc->ndev->dev, "hdmi: scdc scrambling status is reset, "
						"trying to reconfigure.\n");
		_tegra_hdmi_v2_x_config(hdmi);
	}

skip_scdc_i2c:
	/* reschedule the worker */
	cancel_delayed_work(&hdmi->scdc_work);
	schedule_delayed_work(&hdmi->scdc_work,
			msecs_to_jiffies(HDMI_SCDC_MONITOR_TIMEOUT_MS));
}

static void _tegra_hdmi_clock_enable(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	tegra_disp_clk_prepare_enable(sor->safe_clk);
	tegra_hdmi_config_clk(hdmi, TEGRA_HDMI_SAFE_CLK);
	tegra_sor_clk_enable(sor);
}

static void _tegra_hdmi_clock_disable(struct tegra_hdmi *hdmi)
{
	struct tegra_dc_sor_data *sor = hdmi->sor;
	tegra_sor_clk_disable(sor);
}

void tegra_hdmi_get(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	if (atomic_inc_return(&hdmi->clock_refcount) == 1)
		_tegra_hdmi_clock_enable(hdmi);
}

void tegra_hdmi_put(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	if (WARN_ONCE(atomic_read(&hdmi->clock_refcount) <= 0,
		"hdmi: clock refcount imbalance"))
		return;
	if (atomic_dec_return(&hdmi->clock_refcount) == 0)
		_tegra_hdmi_clock_disable(hdmi);
}

/* TODO: add support for other deep colors */
static inline u32 tegra_hdmi_get_bpp(struct tegra_hdmi *hdmi)
{
	int yuv_flag = hdmi->dc->mode.vmode & FB_VMODE_YUV_MASK;

	if ((yuv_flag == (FB_VMODE_Y420 | FB_VMODE_Y30)) ||
		(!(yuv_flag & YUV_MASK) && (yuv_flag == FB_VMODE_Y30)))
		return 30;
	else if (!(yuv_flag & YUV_MASK) && (yuv_flag == FB_VMODE_Y36))
		return 36;
	else if (yuv_flag == (FB_VMODE_Y422 | FB_VMODE_Y36))
		return 24;
	else
		return 0;
}

static u32 tegra_hdmi_gcp_color_depth(struct tegra_hdmi *hdmi)
{
	u32 gcp_cd = 0;

	switch (tegra_hdmi_get_bpp(hdmi)) {
	case 0:
		gcp_cd = TEGRA_HDMI_BPP_UNKNOWN;
		break;
	case 24:
		gcp_cd = TEGRA_HDMI_BPP_24;
		break;
	case 30:
		gcp_cd = TEGRA_HDMI_BPP_30;
		break;
	case 36:
		gcp_cd = TEGRA_HDMI_BPP_36;
		break;
	case 48:
		gcp_cd = TEGRA_HDMI_BPP_48;
		break;
	default:
		dev_WARN(&hdmi->dc->ndev->dev,
			"hdmi: unknown gcp color depth\n");
	};

	return gcp_cd;
}

/* return packing phase of last pixel in preceding video data period */
static u32 tegra_hdmi_gcp_packing_phase(struct tegra_hdmi *hdmi)
{
	int yuv_flag = hdmi->dc->mode.vmode & FB_VMODE_YUV_MASK;

	if (!tegra_hdmi_gcp_color_depth(hdmi))
		return 0;

	if (!(yuv_flag & YUV_MASK) && (yuv_flag == FB_VMODE_Y36))
		return 2;
	else
		return 0;
}

static bool tegra_hdmi_gcp_default_phase_en(struct tegra_hdmi *hdmi)
{
	int yuv_flag = hdmi->dc->mode.vmode & FB_VMODE_YUV_MASK;

	if (!tegra_hdmi_gcp_color_depth(hdmi))
		return false;

	if ((yuv_flag == (FB_VMODE_Y420 | FB_VMODE_Y30)) ||
			(!(yuv_flag & YUV_MASK) && (yuv_flag == FB_VMODE_Y36)))
		return true;
	else
		return false;
}

/* general control packet */
static void tegra_hdmi_gcp(struct tegra_hdmi *hdmi)
{
#define GCP_SB1_PP_SHIFT 4

	struct tegra_dc_sor_data *sor = hdmi->sor;
	u8 sb1, sb2;

	/* disable gcp before configuring */
	tegra_sor_writel(sor, NV_SOR_HDMI_GCP_CTRL, 0);

	sb1 = tegra_hdmi_gcp_packing_phase(hdmi) << GCP_SB1_PP_SHIFT |
		tegra_hdmi_gcp_color_depth(hdmi);
	sb2 = !!tegra_hdmi_gcp_default_phase_en(hdmi);
	tegra_sor_writel(sor, NV_SOR_HDMI_GCP_SUBPACK(0),
			sb1 << NV_SOR_HDMI_GCP_SUBPACK_SB1_SHIFT |
			sb2 << NV_SOR_HDMI_GCP_SUBPACK_SB2_SHIFT);

	/* Send gcp every frame */
	tegra_sor_writel(sor, NV_SOR_HDMI_GCP_CTRL,
			NV_SOR_HDMI_GCP_CTRL_ENABLE |
			NV_SOR_HDMI_GCP_CTRL_OTHER_DIS |
			NV_SOR_HDMI_GCP_CTRL_SINGLE_DIS);

#undef GCP_SB1_PP_SHIFT
}

static int tegra_hdmi_controller_enable(struct tegra_hdmi *hdmi)
{
	struct tegra_dc *dc = hdmi->dc;
	struct tegra_dc_sor_data *sor = hdmi->sor;

	tegra_dc_get(dc);
	tegra_hdmi_get(dc);

	tegra_sor_hdmi_pad_power_up(sor);

	tegra_sor_power_lanes(sor, 4, true);

	tegra_dc_sor_set_internal_panel(sor, false);
	tegra_hdmi_config(hdmi);
	tegra_hdmi_avi_infoframe(hdmi);
	tegra_hdmi_vendor_infoframe(hdmi);

	tegra_sor_pad_cal_power(sor, true);
	tegra_hdmi_config_tmds(hdmi);
	tegra_sor_pad_cal_power(sor, false);

	tegra_hdmi_config_clk(hdmi, TEGRA_HDMI_BRICK_CLK);
	tegra_dc_sor_attach(sor);
	tegra_nvhdcp_set_plug(hdmi->nvhdcp, true);

	tegra_dc_setup_clk(dc, dc->clk);
	tegra_dc_hdmi_setup_clk(dc, hdmi->sor->sor_clk);
	tegra_hdmi_config(hdmi);

	tegra_sor_config_xbar(hdmi->sor);

	/* TODO: Confirm sequence with HW */
	tegra_sor_writel(sor,  NV_SOR_SEQ_INST(0), 0x8080);
	tegra_sor_writel(sor,  NV_SOR_PWR, 0x80000001);

	if (hdmi->dc->mode.pclk > 340000000) {
		tegra_hdmi_v2_x_config(hdmi);
		schedule_delayed_work(&hdmi->scdc_work,
			msecs_to_jiffies(HDMI_SCDC_MONITOR_TIMEOUT_MS));
	}

	tegra_hdmi_gcp(hdmi);

	tegra_dc_put(dc);
	return 0;
}

static void tegra_dc_hdmi_enable(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	if (hdmi->enabled)
		return;

	tegra_hdmi_controller_enable(hdmi);

	hdmi->enabled = true;
	tegra_hda_set_data(hdmi, SINK_HDMI);
#ifdef CONFIG_SWITCH
	if (!hdmi->dvi)
		switch_set_state(&hdmi->audio_switch, 1);
#endif
}

static inline u32 tegra_hdmi_get_shift_clk_div(struct tegra_hdmi *hdmi)
{
	/*
	 * HW does not support deep color yet
	 * always return 0
	 */

	return 0;
}

static void tegra_hdmi_config_clk(struct tegra_hdmi *hdmi, u32 clk_type)
{
	if (clk_type == hdmi->clk_type)
		return;

	if (tegra_platform_is_linsim())
		return;

	if (clk_type == TEGRA_HDMI_BRICK_CLK) {
		u32 val;
		struct tegra_dc_sor_data *sor = hdmi->sor;
		int div = hdmi->dc->mode.pclk < 340000000 ? 1 : 2;
		unsigned long rate = clk_get_rate(sor->src_switch_clk);
		unsigned long parent_rate =
			clk_get_rate(clk_get_parent(sor->src_switch_clk));

		/* Set sor divider */
		if (rate != DIV_ROUND_UP(parent_rate, div)) {
			rate = DIV_ROUND_UP(parent_rate, div);
			clk_set_rate(sor->src_switch_clk, rate);
		}

		/* Select brick muxes */
		val = (hdmi->dc->mode.pclk < 340000000) ?
			NV_SOR_CLK_CNTRL_DP_LINK_SPEED_G2_7 :
			NV_SOR_CLK_CNTRL_DP_LINK_SPEED_G5_4;

		val |= NV_SOR_CLK_CNTRL_DP_CLK_SEL_SINGLE_PCLK;
		tegra_sor_writel(hdmi->sor, NV_SOR_CLK_CNTRL, val);
		usleep_range(250, 300); /* sor brick pll stabilization delay */

		/*
		 * Report brick configuration and rate, so that SOR clock tree
		 * is properly updated. No h/w changes by clock api calls below,
		 * just sync s/w state with brick h/w.
		 */
		rate = rate/NV_SOR_HDMI_BRICK_DIV*NV_SOR_HDMI_BRICK_MUL(val);
		if (clk_get_parent(sor->brick_clk) != sor->src_switch_clk)
			clk_set_parent(sor->brick_clk, sor->src_switch_clk);
		clk_set_rate(sor->brick_clk, rate);

		/*
		 * Select primary -- HDMI -- DVFS table for SOR clock (if SOR
		 * clock has single DVFS table for all modes, nothing changes).
		 */
		tegra_dvfs_use_alt_freqs_on_clk(sor->sor_clk, false);

		/* Select sor clock muxes */
		tegra_clk_cfg_ex(sor->sor_clk, TEGRA_CLK_SOR_CLK_SEL, 3);

		tegra_dc_writel(hdmi->dc, PIXEL_CLK_DIVIDER_PCD1 |
			SHIFT_CLK_DIVIDER(tegra_hdmi_get_shift_clk_div(hdmi)),
			DC_DISP_DISP_CLOCK_CONTROL);

		hdmi->clk_type = TEGRA_HDMI_BRICK_CLK;
	} else if (clk_type == TEGRA_HDMI_SAFE_CLK) {
		if (!hdmi->dc->initialized) {
			/* Select sor clock muxes */
			tegra_clk_cfg_ex(hdmi->sor->sor_clk,
				TEGRA_CLK_SOR_CLK_SEL, 0);
			hdmi->clk_type = TEGRA_HDMI_SAFE_CLK;
		}
	} else {
		dev_err(&hdmi->dc->ndev->dev, "hdmi: incorrect clk type configured\n");
	}
}

/* returns exact pixel clock in Hz */
static long tegra_hdmi_get_pclk(struct tegra_dc_mode *mode)
{
	long h_total, v_total;
	long refresh, pclk;
	h_total = mode->h_active + mode->h_front_porch + mode->h_back_porch +
		mode->h_sync_width;
	v_total = mode->v_active + mode->v_front_porch + mode->v_back_porch +
		mode->v_sync_width;
	refresh = tegra_dc_calc_refresh(mode);
	refresh = DIV_ROUND_CLOSEST(refresh, 1000);

	pclk = h_total * v_total * refresh;

	if (mode->vmode & FB_VMODE_1000DIV1001)
		pclk = pclk * 1000 / 1001;

	return pclk;
}

static long tegra_dc_hdmi_setup_clk(struct tegra_dc *dc, struct clk *clk)
{
#ifdef CONFIG_TEGRA_NVDISPLAY
	struct clk *parent_clk = tegra_disp_clk_get(&dc->ndev->dev,
				dc->out->parent_clk ? : "plld2");
#else
	struct clk *parent_clk = clk_get(NULL,
				dc->out->parent_clk ? : "pll_d2");
#endif

	dc->mode.pclk = tegra_hdmi_get_pclk(&dc->mode);

	if (IS_ERR_OR_NULL(parent_clk)) {
		dev_err(&dc->ndev->dev, "hdmi: parent clk get failed\n");
		return 0;
	}

	if (!tegra_platform_is_silicon())
		return dc->mode.pclk;

#ifdef CONFIG_TEGRA_NVDISPLAY
	if (clk_get_parent(clk) != parent_clk)
		clk_set_parent(clk, parent_clk);
#else
	if (clk == dc->clk) {
		if (clk_get_parent(clk) != parent_clk) {
			if (clk_set_parent(clk, parent_clk)) {
				dev_err(&dc->ndev->dev,
					"hdmi: set dc parent failed\n");
				return 0;
			}
		}
	} else {
		struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);
		struct tegra_dc_sor_data *sor = hdmi->sor;

		if (clk_get_parent(sor->src_switch_clk) != parent_clk) {
			if (clk_set_parent(sor->src_switch_clk, parent_clk)) {
				dev_err(&dc->ndev->dev,
					"hdmi: set src switch parent failed\n");
				return 0;
			}
		}
	}
#endif
	if (dc->initialized)
		goto skip_setup;
	if (clk_get_rate(parent_clk) != dc->mode.pclk)
		clk_set_rate(parent_clk, dc->mode.pclk);
skip_setup:
	/*
	 * DC clock divider is controlled by DC driver transparently to clock
	 * framework -- hence, direct call to DVFS with target mode rate. SOR
	 * clock rate in clock tree is properly updated, and can be used for
	 * DVFS update.
	 *
	 * TODO: tegra_hdmi_controller_enable() procedure 1st configures SOR
	 * clock via tegra_hdmi_config_clk(), and then calls this function
	 * that may re-lock parent PLL. That needs to be double-checked:
	 * in general re-locking PLL while the downstream module is already
	 * sourced from it is not recommended. If/when the order of enabling
	 * HDMI controller is changed, we can remove direct DVFS call for SOR
	 * (but for DC it should be kept, anyway).
	 */
	if (clk == dc->clk)
		tegra_dvfs_set_rate(clk, dc->mode.pclk);
	else
		tegra_dvfs_set_rate(clk, clk_get_rate(clk));

	return tegra_dc_pclk_round_rate(dc, dc->mode.pclk);
}

static void tegra_dc_hdmi_shutdown(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	_tegra_hdmivrr_activate(hdmi, false);
	hdmi->device_shutdown = true;
	tegra_nvhdcp_shutdown(hdmi->nvhdcp);

	return;
}

static void tegra_dc_hdmi_disable(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	_tegra_hdmivrr_activate(hdmi, false);
	hdmi->enabled = false;
#ifdef CONFIG_SWITCH
	switch_set_state(&hdmi->audio_switch, 0);
#endif

	tegra_hdmi_config_clk(hdmi, TEGRA_HDMI_SAFE_CLK);
	tegra_hdmi_controller_disable(hdmi);
	tegra_hda_reset_data();
	return;
}

static bool tegra_dc_hdmi_detect(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);
	unsigned long delay = msecs_to_jiffies(HDMI_HPD_DEBOUNCE_DELAY_MS);

	if (tegra_platform_is_linsim())
		return true;

	if (dc->out->hotplug_state != TEGRA_HPD_STATE_NORMAL)
		delay = 0;

	cancel_delayed_work(&hdmi->hpd_worker);
	schedule_delayed_work(&hdmi->hpd_worker, delay);

	return tegra_dc_hpd(dc);
}

static void tegra_dc_hdmi_suspend(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	_tegra_hdmivrr_activate(hdmi, false);

	if (dc->out->flags & TEGRA_DC_OUT_HOTPLUG_WAKE_LP0) {
		int wake_irq = gpio_to_irq(dc->out->hotplug_gpio);
		int ret;

		ret = enable_irq_wake(wake_irq);
		if (ret < 0) {
			dev_err(&dc->ndev->dev,
			"%s: Couldn't enable HDMI wakeup, irq=%d, error=%d\n",
			__func__, wake_irq, ret);
		}
	}

	atomic_set(&hdmi->suspended, 1);
}

static void tegra_dc_hdmi_resume(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	atomic_set(&hdmi->suspended, 0);

	if (dc->out->flags & TEGRA_DC_OUT_HOTPLUG_WAKE_LP0)
		disable_irq_wake(gpio_to_irq(dc->out->hotplug_gpio));

	cancel_delayed_work(&hdmi->hpd_worker);

	/* If resume happens with a non-VRR monitor, the HPD
	   worker will correct the mode based on the new EDID */
	_tegra_hdmivrr_activate(hdmi, true);

	schedule_delayed_work(&hdmi->hpd_worker,
				msecs_to_jiffies(HDMI_HPD_DEBOUNCE_DELAY_MS + HDMI_HPD_DROP_TIMEOUT_MS));
}

static int tegra_dc_hdmi_set_hdr(struct tegra_dc *dc)
{
	u16 ret = 0;
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	/* If the sink isn't HDR capable, return */
	ret = tegra_edid_get_ex_hdr_cap(hdmi->edid);
	if (!ret)
		return 0;

	/* Cancel any pending hdr-exit work */
	if (dc->hdr.enabled)
		cancel_delayed_work_sync(&hdmi->hdr_worker);

	tegra_hdmi_hdr_infoframe(hdmi);

	/*
	 *If hdr is disabled then send HDR infoframe for
	 *2 secs with SDR eotf and then stop
	 */
	if (dc->hdr.enabled)
		return 0;

	schedule_delayed_work(&hdmi->hdr_worker,
		msecs_to_jiffies(HDMI_HDR_INFOFRAME_STOP_TIMEOUT_MS));

	return 0;
}

static void tegra_hdmi_hdr_worker(struct work_struct *work)
{
	u32 val = 0;
	struct tegra_hdmi *hdmi = container_of(to_delayed_work(work),
				struct tegra_hdmi, hdr_worker);

	if (hdmi && hdmi->enabled) {
		/* If hdr re-enabled within 2s, return.
		 * Note this an extra safety check since
		 * we should have already cancelled this work */
		if (hdmi->dc->hdr.enabled)
			return;
		/* Read the current regsiter value to restore the bits */
		val = tegra_sor_readl(hdmi->sor, NV_SOR_HDMI_GENERIC_CTRL);

		/* Set val to disable generic infoframe */
		val &= ~NV_SOR_HDMI_GENERIC_CTRL_ENABLE_YES;

		tegra_sor_writel(hdmi->sor, NV_SOR_HDMI_GENERIC_CTRL, val);
	}
	return;
}

static int tegra_dc_hdmi_ddc_enable(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);
	_tegra_hdmi_ddc_enable(hdmi);
	return 0;
}

static int tegra_dc_hdmi_ddc_disable(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);
	_tegra_hdmi_ddc_disable(hdmi);
	return 0;
}

static void tegra_dc_hdmi_modeset_notifier(struct tegra_dc *dc)
{
	struct tegra_hdmi *hdmi = tegra_dc_get_outdata(dc);

	tegra_hdmi_get(dc);
	tegra_dc_io_start(dc);

	/* disable hdmi2.x config on host and monitor */
	if (dc->mode.pclk > 340000000) {
		if (tegra_edid_is_scdc_present(dc->edid))
			tegra_hdmi_v2_x_mon_config(hdmi, true);
		tegra_hdmi_v2_x_host_config(hdmi, true);
	} else {
		if (tegra_edid_is_scdc_present(dc->edid))
			tegra_hdmi_v2_x_mon_config(hdmi, false);
		tegra_hdmi_v2_x_host_config(hdmi, false);
	}

	if (!(dc->mode.vmode & FB_VMODE_SET_YUV_MASK))
		_tegra_dc_cmu_enable(dc,
			dc->mode.vmode & FB_VMODE_LIMITED_RANGE);

	tegra_dc_io_end(dc);
	tegra_hdmi_put(dc);
}

#ifdef CONFIG_DEBUG_FS
/* show current hpd state */
static int tegra_hdmi_hotplug_dbg_show(struct seq_file *m, void *unused)
{
	struct tegra_hdmi *hdmi = m->private;
	struct tegra_dc *dc = hdmi->dc;

	if (WARN_ON(!hdmi || !dc || !dc->out))
		return -EINVAL;

	rmb();
	seq_printf(m, "hdmi hpd state: %d\n", dc->out->hotplug_state);

	return 0;
}

int tegra_hdmi_get_hotplug_state(struct tegra_hdmi *hdmi)
{
	rmb();
	return hdmi->dc->out->hotplug_state;
}

void tegra_hdmi_set_hotplug_state(struct tegra_hdmi *hdmi, int new_hpd_state)
{
	struct tegra_dc *dc = hdmi->dc;
	int hotplug_state;

	rmb();
	hotplug_state = dc->out->hotplug_state;

	if (hotplug_state == TEGRA_HPD_STATE_NORMAL &&
			new_hpd_state != TEGRA_HPD_STATE_NORMAL &&
			tegra_dc_hotplug_supported(dc)) {
		disable_irq(gpio_to_irq(dc->out->hotplug_gpio));
	} else if (hotplug_state != TEGRA_HPD_STATE_NORMAL &&
			new_hpd_state == TEGRA_HPD_STATE_NORMAL &&
			tegra_dc_hotplug_supported(dc)) {
		enable_irq(gpio_to_irq(dc->out->hotplug_gpio));
	}

	dc->out->hotplug_state = new_hpd_state;
	wmb();

	/*
	 * sw controlled plug/unplug.
	 * wait for any already executing hpd worker thread.
	 * No debounce delay, schedule immedately
	 */
	cancel_delayed_work_sync(&hdmi->hpd_worker);
	schedule_delayed_work(&hdmi->hpd_worker, 0);
}

/*
 * sw control for hpd.
 * 0 is normal state, hw drives hpd.
 * -1 is force deassert, sw drives hpd.
 * 1 is force assert, sw drives hpd.
 * before releasing to hw, sw must ensure hpd state is normal i.e. 0
 */
static ssize_t tegra_hdmi_hotplug_dbg_write(struct file *file,
					const char __user *addr,
					size_t len, loff_t *pos)
{
	struct seq_file *m = file->private_data;
	struct tegra_hdmi *hdmi = m->private;
	struct tegra_dc *dc = hdmi->dc;
	long new_hpd_state;
	int ret;

	if (WARN_ON(!hdmi || !dc || !dc->out))
		return -EINVAL;

	ret = kstrtol_from_user(addr, len, 10, &new_hpd_state);
	if (ret < 0)
		return ret;

	tegra_hdmi_set_hotplug_state(hdmi, new_hpd_state);

	return len;
}

static int tegra_hdmi_hotplug_dbg_open(struct inode *inode, struct file *file)
{
	return single_open(file, tegra_hdmi_hotplug_dbg_show, inode->i_private);
}

static const struct file_operations tegra_hdmi_hotplug_dbg_ops = {
	.open = tegra_hdmi_hotplug_dbg_open,
	.read = seq_read,
	.write = tegra_hdmi_hotplug_dbg_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tegra_hdmi_status_dbg_show(struct seq_file *m, void *unused)
{
	struct tegra_hdmi *hdmi = m->private;
	struct tegra_dc *dc;

	if (WARN_ON(!hdmi))
		return -EINVAL;

	dc = hdmi->dc;

	if (WARN_ON(!dc || !dc->out))
		return -EINVAL;

	seq_printf(m, "hotplug state: %d\n", tegra_dc_hpd(dc));
	seq_printf(m, "SCDC present: %d\n",
		tegra_edid_is_scdc_present(hdmi->edid));
	seq_printf(m, "Forum VSDB present: %d\n",
		tegra_edid_is_hfvsdb_present(hdmi->edid));
	seq_printf(m, "YCbCr4:2:0 VDB present: %d\n",
		tegra_edid_is_420db_present(hdmi->edid));

	return 0;
}

static int tegra_hdmi_status_dbg_open(struct inode *inode, struct file *file)
{
	return single_open(file, tegra_hdmi_status_dbg_show,
		inode->i_private);
}

static const struct file_operations tegra_hdmi_status_dbg_ops = {
	.open = tegra_hdmi_status_dbg_open,
	.read = seq_read,
	.release = single_release,
};

static void tegra_hdmi_debugfs_init(struct tegra_hdmi *hdmi)
{
	struct dentry *dir, *ret;

	dir = debugfs_create_dir("tegra_hdmi",  NULL);
	if (IS_ERR_OR_NULL(dir))
		return;

	ret = debugfs_create_file("hotplug", S_IRUGO, dir,
				hdmi, &tegra_hdmi_hotplug_dbg_ops);
	if (IS_ERR_OR_NULL(ret))
		goto fail;
	ret = debugfs_create_file("hdmi_status", S_IRUGO, dir,
				hdmi, &tegra_hdmi_status_dbg_ops);
	if (IS_ERR_OR_NULL(ret))
		goto fail;
	return;
fail:
	debugfs_remove_recursive(dir);
	return;
}
#else
static void tegra_hdmi_debugfs_init(struct tegra_hdmi *hdmi)
{
	return;
}
#endif

static ssize_t hdmi_ddc_power_toggle(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int value;
	sscanf(buf, "%du", &value);
	tegra_hdmi_ddc_power_toggle(value);
	return count;
}

static ssize_t hdmi_ddc_power_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", dc_hdmi->ddc_refcount);
}

static bool tegra_dc_hdmi_hpd_state(struct tegra_dc *dc)
{
	int sense;
	int level;
	bool hpd;

	if (WARN_ON(!dc || !dc->out))
		return false;

	level = gpio_get_value_cansleep(dc->out->hotplug_gpio);

	sense = dc->out->flags & TEGRA_DC_OUT_HOTPLUG_MASK;

	hpd = (sense == TEGRA_DC_OUT_HOTPLUG_HIGH && level) ||
		(sense == TEGRA_DC_OUT_HOTPLUG_LOW && !level);

	return hpd;
}

static void tegra_dc_hdmi_vrr_enable(struct tegra_dc *dc, bool enable)
{
	struct tegra_vrr *vrr  = dc->out->vrr;

	if (!vrr)
		return;

	if (!(dc->mode.vmode & FB_VMODE_VRR)) {
		WARN(enable, "VRR enable request in non-VRR mode\n");
		return;
	}

	vrr->enable = enable;
}

static void tegra_dc_hdmi_postpoweron(struct tegra_dc *dc)
{
	_tegra_hdmivrr_activate(tegra_dc_get_outdata(dc), true);
}

struct tegra_dc_out_ops tegra_dc_hdmi2_0_ops = {
	.init = tegra_dc_hdmi_init,
	.destroy = tegra_dc_hdmi_destroy,
	.enable = tegra_dc_hdmi_enable,
	.disable = tegra_dc_hdmi_disable,
	.setup_clk = tegra_dc_hdmi_setup_clk,
	.detect = tegra_dc_hdmi_detect,
	.shutdown = tegra_dc_hdmi_shutdown,
	.suspend = tegra_dc_hdmi_suspend,
	.resume = tegra_dc_hdmi_resume,
	.ddc_enable = tegra_dc_hdmi_ddc_enable,
	.ddc_disable = tegra_dc_hdmi_ddc_disable,
	.modeset_notifier = tegra_dc_hdmi_modeset_notifier,
	.mode_filter = tegra_hdmi_fb_mode_filter,
	.hpd_state = tegra_dc_hdmi_hpd_state,
	.vrr_enable = tegra_dc_hdmi_vrr_enable,
	.vrr_update_monspecs = tegra_hdmivrr_update_monspecs,
	.set_hdr = tegra_dc_hdmi_set_hdr,
	.postpoweron = tegra_dc_hdmi_postpoweron,
};
