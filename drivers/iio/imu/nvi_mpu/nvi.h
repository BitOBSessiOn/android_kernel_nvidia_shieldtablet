/* Copyright (c) 2014-2016, NVIDIA CORPORATION.  All rights reserved.
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

/* NVS = NVidia Sensor framework */
/* See nvs_iio.c and nvs.h for documentation */

#ifndef _NVI_H_
#define _NVI_H_

#include <asm/atomic.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/regulator/consumer.h>
#include <linux/mpu_iio.h>
#include <linux/nvs.h>

#define NVI_BYPASS_TIMEOUT_MS		(1000)
#define POWER_UP_TIME			(100)
#define REG_UP_TIME			(5)
#define POR_MS				(100)
#define GYRO_STARTUP_DELAY_NS		(100000000) /* 100ms */
#define NVI_IRQ_STORM_MIN_NS		(1000000) /* storm if irq faster 1ms */
#define NVI_IRQ_STORM_MAX_N		(100) /* max storm irqs b4 dis irq */
#define NVI_FIFO_SAMPLE_SIZE_MAX	(38)
#define KBUF_SZ				(64)
#define SRC_MPU				(0)
#define SRC_GYR				(0)
#define SRC_ACC				(1)
#define SRC_AUX				(2)
#define SRC_N				(3)

#define AXIS_X				(0)
#define AXIS_Y				(1)
#define AXIS_Z				(2)
#define AXIS_N				(3)
#define DEV_ACC				(0)
#define DEV_GYR				(1)
#define DEV_AXIS_N			(2)
#define DEV_TMP				(2)
#define DEV_MPU_N			(3)
#define DEV_SM				(3)
#define DEV_STP				(4)
#define DEV_QTN				(5)
#define DEV_N				(6)
#define DEV_AUX				(6)
#define DEV_N_AUX			(7)
#define DEV_DMP				(8)
#define FW_LOADED			(16)
#define EN_STDBY			(17)
#define EN_LP				(18)
#define MSK_RST				((1 << EN_STDBY) | \
					 (1 << EN_LP))
#define MSK_DEV_MPU			((1 << DEV_ACC) | \
					 (1 << DEV_GYR) | \
					 (1 << DEV_TMP))
#define MSK_DEV_DMP			((1 << DEV_SM) | \
					 (1 << DEV_STP) | \
					 (1 << DEV_QTN))
#define MSK_DEV_SNSR			(MSK_DEV_MPU | MSK_DEV_DMP)
#define MSK_DEV_ALL			(MSK_DEV_SNSR | (1 << DEV_AUX))
#define MSK_PM_ON_FULL			(1 << DEV_GYR)
#define MSK_PM_ON			((1 << DEV_TMP) | \
					 (1 << DEV_AUX) | \
					 MSK_DEV_DMP)
#define MSK_PM_LP			((1 << EN_LP) | (1 << DEV_ACC))
#define MSK_PM_STDBY			((1 << EN_STDBY) | (1 << FW_LOADED))
#define NVI_PM_ERR			(0)
#define NVI_PM_AUTO			(1)
#define NVI_PM_OFF_FORCE		(2)
#define NVI_PM_OFF			(3)
#define NVI_PM_STDBY			(4)
#define NVI_PM_ON_CYCLE			(5)
#define NVI_PM_ON			(6)
#define NVI_PM_ON_FULL			(7)
#define INV_CLK_INTERNAL		(0)
#define INV_CLK_PLL			(1)

#define NVI_DBG_SPEW_MSG		(1 << NVS_STS_EXT_N)
#define NVI_DBG_SPEW_AUX		(1 << (NVS_STS_EXT_N + 1))
#define NVI_DBG_SPEW_FIFO		(1 << (NVS_STS_EXT_N + 2))
#define NVI_DBG_SPEW_TS			(1 << (NVS_STS_EXT_N + 3))
#define NVI_DBG_SPEW_SNSR		(1 << (NVS_STS_EXT_N + 4))
/* register bits */
#define BITS_SELF_TEST_EN		(0xE0)
#define BIT_ACCEL_FCHOCIE_B		(0x08)
#define BIT_FIFO_SIZE_1K		(0x40)
#define BITS_GYRO_OUT			(0x70)
#define BIT_I2C_MST_P_NSR		(0x10)
#define BIT_I2C_READ			(0x80)
#define BITS_I2C_SLV_CTRL_LEN		(0x0F)
#define BIT_I2C_SLV_REG_DIS		(0x10)
#define BIT_SLV_EN			(0x80)
#define BITS_I2C_MST_DLY		(0x1F)
#define BIT_BYPASS_EN			(0x02)
#define BIT_DATA_RDY_EN			(0x01)
#define BIT_DMP_INT_EN			(0x02)
#define BIT_FIFO_OVERFLOW		(0x10)
#define BIT_ZMOT_EN			(0x20)
#define BIT_MOT_EN			(0x40)
#define BIT_6500_WOM_EN			(0x40)
#define BIT_SLV0_DLY_EN			(0x01)
#define BIT_SLV1_DLY_EN			(0x02)
#define BIT_SLV2_DLY_EN			(0x04)
#define BIT_SLV3_DLY_EN			(0x08)
#define BIT_DELAY_ES_SHADOW		(0x80)
#define BIT_ACCEL_INTEL_MODE		(0x40)
#define BIT_ACCEL_INTEL_ENABLE		(0x80)
#define BITS_USER_CTRL_RST		(0x0F)
#define BIT_SIG_COND_RST		(0x01)
#define BIT_I2C_MST_RST			(0x02)
#define BIT_FIFO_RST			(0x04)
#define BIT_DMP_RST			(0x08)
#define BIT_I2C_MST_EN			(0x20)
#define BIT_FIFO_EN			(0x40)
#define BIT_DMP_EN			(0x80)
#define BIT_CLK_MASK			(0x07)
#define BIT_CYCLE			(0x20)
#define BIT_SLEEP			(0x40)
#define BIT_H_RESET			(0x80)
#define BIT_PWR_GYRO_STBY		(0x07)
#define BIT_PWR_ACCEL_STBY		(0x38)
#define BIT_PWR_PRESSURE_STBY		(0x40)
#define BIT_LPA_FREQ			(0xC0)

#define AUX_PORT_MAX			(5)
#define AUX_PORT_IO			(4)
#define AUX_EXT_DATA_REG_MAX		(24)
#define AUX_DEV_VALID_READ_LOOP_MAX	(20)
#define AUX_DEV_VALID_READ_DELAY_MS	(5)

#define DMP_HDR_LEN_MAX			(4)

struct nvi_state;

struct nvi_rr {
	struct nvs_float max_range;
	struct nvs_float resolution;
};

struct nvi_hal_dev {
	int version;
	int src;
	unsigned int rr_0n;
	struct nvi_rr *rr;
	struct nvs_float scale;
	struct nvs_float offset;
	struct nvs_float milliamp;
	u16 fifo_en_msk;
	int fifo;
	unsigned int fifo_data_n;
};

struct nvi_hal_src {
	unsigned int dev_msk;
	unsigned int period_us_min;
	unsigned int period_us_max;
	int (*fn_period)(struct nvi_state *st);
};

struct nvi_src {
	bool ts_reset;
	s64 ts_1st;
	s64 ts_end;
	s64 ts_period;
	unsigned int period_us_src;
	unsigned int period_us_req;
	unsigned int fifo_data_n;
	u32 base_t;
};

struct nvi_br {
	u8 bank;
	u8 reg;
	u8 len;
	u32 dflt;
};

struct nvi_hal_reg {
	struct nvi_br self_test_g[AXIS_N];
	struct nvi_br self_test_a[AXIS_N];
	struct nvi_br g_offset_h[AXIS_N];
	struct nvi_br a_offset_h[AXIS_N];
	struct nvi_br tbc_pll;
	struct nvi_br tbc_rcosc;
	struct nvi_br smplrt[SRC_N];
	struct nvi_br gyro_config1;
	struct nvi_br gyro_config2;
	struct nvi_br accel_config;
	struct nvi_br accel_config2;
	struct nvi_br lp_config;
	struct nvi_br int_pin_cfg;
	struct nvi_br int_enable;
	struct nvi_br int_dmp;
	struct nvi_br int_status;
	struct nvi_br out_h[DEV_MPU_N];
	struct nvi_br ext_sens_data_00;
	struct nvi_br signal_path_reset;
	struct nvi_br user_ctrl;
	struct nvi_br pm1;
	struct nvi_br pm2;
	struct nvi_br fifo_en;
	struct nvi_br fifo_rst;
	struct nvi_br fifo_sz;
	struct nvi_br fifo_count_h;
	struct nvi_br fifo_rw;
	struct nvi_br fifo_cfg;
	struct nvi_br who_am_i;
	struct nvi_br i2c_mst_status;
	struct nvi_br i2c_mst_odr_config;
	struct nvi_br i2c_mst_ctrl;
	struct nvi_br i2c_mst_delay_ctrl;
	struct nvi_br i2c_slv_addr[AUX_PORT_MAX];
	struct nvi_br i2c_slv_reg[AUX_PORT_MAX];
	struct nvi_br i2c_slv_ctrl[AUX_PORT_IO];
	struct nvi_br i2c_slv4_ctrl;
	struct nvi_br i2c_slv_do[AUX_PORT_MAX];
	struct nvi_br i2c_slv4_di;
	struct nvi_br mem_addr;
	struct nvi_br mem_rw;
	struct nvi_br mem_bank;
	struct nvi_br fw_start;
	struct nvi_br reg_bank;
};

struct nvi_hal_bit {
	u8 int_i2c_mst;
	u8 int_dmp;
	u8 int_pll_rdy;
	u8 int_wom;
	u8 int_wof;
	u8 int_data_rdy_0;
	u8 int_data_rdy_1;
	u8 int_data_rdy_2;
	u8 int_data_rdy_3;
	u8 int_fifo_ovrflw_0;
	u8 int_fifo_ovrflw_1;
	u8 int_fifo_ovrflw_2;
	u8 int_fifo_ovrflw_3;
	u8 int_fifo_wm_0;
	u8 int_fifo_wm_1;
	u8 int_fifo_wm_2;
	u8 int_fifo_wm_3;
	u8 slv_fifo_en[AUX_PORT_IO];
};

struct nvi_rc {
	u16 accel_offset[AXIS_N];
	u16 gyro_offset[AXIS_N];
	u16 smplrt[SRC_N];
	u8 gyro_config1;
	u8 gyro_config2;
	u8 accel_config;
	u8 accel_config2;
	u8 lp_config;
	u8 int_pin_cfg;
	u32 int_enable;
	u32 int_status;
	u8 i2c_mst_odr_config;
	u8 i2c_mst_ctrl;
	u8 i2c_mst_delay_ctrl;
	u8 i2c_slv_addr[AUX_PORT_MAX];
	u8 i2c_slv_reg[AUX_PORT_MAX];
	u8 i2c_slv_ctrl[AUX_PORT_IO];
	u8 i2c_slv4_ctrl;
	u8 i2c_slv_do[AUX_PORT_MAX];
	u8 user_ctrl;
	u8 pm1;
	u8 pm2;
	u16 fifo_en;
	u8 fifo_sz;
	u8 fifo_cfg;
	u8 reg_bank;
};

#define DMP_DEV_ABLE_LEN		(2)

struct nvi_dmp_icm {
	u16 en_addr;
	u16 en_msk;
	u16 odr_cfg;
	u16 odr_cntr;
};

struct nvi_dmp_mpu {
	u16 en_addr;
	u8 en_len;
	u8 en[DMP_DEV_ABLE_LEN];
	u8 dis[DMP_DEV_ABLE_LEN];
	u16 odr_cfg;
	u16 odr_cntr;
};

struct nvi_dmp_dev {
	unsigned int dev;
	unsigned int data_n;
	unsigned int aux_port;
	unsigned int hdr_n;
	u8 hdr[DMP_HDR_LEN_MAX];
	u8 hdr_msk[DMP_HDR_LEN_MAX];
	int (*fn_init)(struct nvi_state *st);
	int (*fn_push)(struct nvi_state *st, u8 *buf);
	union {
		struct nvi_dmp_icm icm;
		struct nvi_dmp_mpu mpu;
	};
};

struct nvi_dmp {
	const u8 const *fw;
	unsigned int fw_len;
	unsigned int fw_crc32;
	unsigned int fw_mem_addr;
	unsigned int fw_start;
	unsigned int dmp_period_us;
	unsigned int dev_msk;
	unsigned int en_msk;
	unsigned int dd_n;
	const struct nvi_dmp_dev *dd;
	int (*fn_init)(struct nvi_state *st);
	int (*fn_en)(struct nvi_state *st);
	int (*fn_dev_init)(struct nvi_state *st, unsigned int dev);
	int (*fn_dev_enable)(struct nvi_state *st, unsigned int dev, int port);
	int (*fn_dev_batch)(struct nvi_state *st, unsigned int dev, int port);
};

struct nvi_fn {
	void (*por2rc)(struct nvi_state *st);
	int (*pm)(struct nvi_state *st, u8 pm1, u8 pm2, u8 lp);
	int (*init)(struct nvi_state *st);
	int (*st_acc)(struct nvi_state *st);
	int (*st_gyr)(struct nvi_state *st);
	int (*en_acc)(struct nvi_state *st);
	int (*en_gyr)(struct nvi_state *st);
};

struct nvi_hal {
	unsigned int regs_n;
	unsigned int reg_bank_n;
	const struct nvi_hal_src *src;
	unsigned int src_n;
	int *fifo_dev;
	unsigned int fifo_n;
	const unsigned long *lp_tbl;
	unsigned int lp_tbl_n;
	const struct nvi_hal_dev *dev[DEV_N_AUX];
	const struct nvi_hal_reg *reg;
	const struct nvi_hal_bit *bit;
	struct nvi_fn *fn;
	struct nvi_dmp *dmp;
};

struct nvi_snsr {
	void *nvs_st;
	struct sensor_cfg cfg;
	unsigned int usr_cfg;
	unsigned int enable;
	unsigned int period_us;
	unsigned int timeout_us;
	unsigned int fsync;
	unsigned int ts_n;
	s64 ts_push_delay;
	s64 push_delay_ns;
	s64 ts_last;
	bool ts_reset;
	bool flush;
};

struct aux_port {
	struct nvi_mpu_port nmp;
	unsigned short ext_data_offset;
	bool hw_valid;
	bool hw_en;
	bool hw_do;
	bool flush;
	unsigned int period_us;
	unsigned int timeout_us;
};

struct aux_ports {
	struct aux_port port[AUX_PORT_MAX];
	s64 bypass_timeout_ns;
	unsigned int bypass_lock;
	u8 delay_hw;
	unsigned short ext_data_n;
	unsigned char ext_data[AUX_EXT_DATA_REG_MAX];
	unsigned char clock_i2c;
	bool reset_i2c;
	bool reset_fifo;
};

/**
 *  struct inv_chip_info_s - Chip related information.
 *  @product_id:	Product id.
 *  @product_revision:	Product revision.
 *  @silicon_revision:	Silicon revision.
 *  @software_revision:	software revision.
 *  @multi:		accel specific multiplier.
 *  @gyro_sens_trim:	Gyro sensitivity trim factor.
 *  @accel_sens_trim:    accel sensitivity trim factor.
 */
struct inv_chip_info_s {
	u8 product_id;
	u8 product_revision;
	u8 silicon_revision;
	u8 software_revision;
	u8 multi;
	u32 gyro_sens_trim;
	u32 accel_sens_trim;
};

struct nvi_state {
	struct i2c_client *i2c;
	struct nvs_fn_if *nvs;
	struct regulator_bulk_data vreg[2];
	struct notifier_block nb_vreg[2];
	const struct nvi_hal *hal;
	struct nvi_rc rc;
	struct aux_ports aux;
	unsigned int sts;
	unsigned int errs;
	unsigned int info;
	unsigned int en_msk;
	struct nvi_snsr snsr[DEV_N_AUX];
	struct nvi_src src[SRC_N];
	int fifo_src;

	unsigned int src_timeout_us[SRC_N];

	bool rc_dis;
	bool irq_dis;
	bool irq_set_irq_wake;
	int pm;
	s64 ts_vreg_en[2];
	atomic64_t ts_irq;

	struct inv_chip_info_s chip_info;
	int bias[DEV_AXIS_N][AXIS_N];
	s16 dev_offset[DEV_AXIS_N][AXIS_N];
	s16 rom_offset[DEV_AXIS_N][AXIS_N];
	u8 st_data[DEV_AXIS_N][AXIS_N];

	unsigned int bypass_timeout_ms;
	unsigned int irq_storm_n;
	unsigned int buf_i;
	u8 buf[NVI_FIFO_SAMPLE_SIZE_MAX * 2]; /* (* 2)=FIFO OVERFLOW OFFSET */
};

int nvi_i2c_wr(struct nvi_state *st, const struct nvi_br *br,
	       u8 val, const char *fn);
int nvi_i2c_wr_rc(struct nvi_state *st, const struct nvi_br *br,
		  u8 val, const char *fn, u8 *rc);
int nvi_i2c_write_rc(struct nvi_state *st, const struct nvi_br *br, u32 val,
		     const char *fn, u8 *rc, bool be);
int nvi_i2c_r(struct nvi_state *st, u8 bank, u8 reg, u16 len, u8 *buf);
int nvi_i2c_rd(struct nvi_state *st, const struct nvi_br *br, u8 *buf);
int nvi_mem_wr(struct nvi_state *st, u16 mem_addr, u16 len, u8 *data,
	       bool validate);
int nvi_mem_wr_be(struct nvi_state *st, u16 mem_addr, u16 len, u32 val);
int nvi_mem_rd(struct nvi_state *st, u16 mem_addr, u16 len, u8 *data);
int nvi_wr_accel_offset(struct nvi_state *st, unsigned int axis, u16 offset);
int nvi_wr_gyro_offset(struct nvi_state *st, unsigned int axis, u16 offset);
int nvi_wr_fifo_cfg(struct nvi_state *st, int fifo);
int nvi_int_able(struct nvi_state *st, const char *fn, bool enable);
int nvi_reset(struct nvi_state *st, const char *fn,
	      bool rst_fifo, bool rst_i2c);
int nvi_user_ctrl_en(struct nvi_state *st, const char *fn,
		     bool en_dmp, bool en_fifo, bool en_i2c, bool en_irq);
int nvi_wr_pm1(struct nvi_state *st, const char *fn, u8 pm1);
int nvi_pm_wr(struct nvi_state *st, const char *fn, u8 pm1, u8 pm2, u8 lp);
int nvi_pm(struct nvi_state *st, const char *fn, int pm_req);
int nvi_aux_delay(struct nvi_state *st, const char *fn);

extern const struct nvi_hal nvi_hal_20628;
extern const struct nvi_hal nvi_hal_6515;
extern const struct nvi_hal nvi_hal_6500;
extern const struct nvi_hal nvi_hal_6050;
extern struct nvi_dmp nvi_dmp_icm;
extern struct nvi_dmp nvi_dmp_mpu;

#endif /* _NVI_H_ */

