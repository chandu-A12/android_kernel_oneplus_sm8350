// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#define pr_fmt(fmt)	"[sy6970]:%s: " fmt, __func__

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/math64.h>
#include <linux/power_supply.h>
#include <linux/iio/consumer.h>

#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>

#include <../../../../include/soc/oplus/boot_mode.h>
#include "../oplus_charger.h"
#include "../oplus_gauge.h"
#include "../oplus_vooc.h"
#include "../oplus_short.h"
#include "../oplus_adapter.h"
#include "../charger_ic/oplus_short_ic.h"
#include "../gauge_ic/oplus_bq27541.h"
// #include "../oplus_chg_module.h"
#include "../oplus_chg_ops_manager.h"
#include "oplus_discrete_charger.h"
#include "oplus_sy6970_reg.h"

#ifdef CONFIG_OPLUS_CHARGER_MTK
extern struct charger_consumer *charger_manager_get_by_name(
		struct device *dev,	const char *name);
extern int mt6357_get_vbus_voltage(void);
extern bool mt6357_get_vbus_status(void);
extern int oplus_battery_meter_get_battery_voltage(void);
extern int oplus_get_rtc_ui_soc(void);
extern int oplus_set_rtc_ui_soc(int value);
extern int set_rtc_spare_fg_value(int val);

extern void mt_usb_connect(void);
extern void mt_usb_disconnect(void);
extern bool mt6357_chrdet_status(void);
#else
extern int smbchg_get_boot_reason(void);
extern int qpnp_get_battery_voltage(void);
extern int oplus_chg_get_shutdown_soc(void);
extern int oplus_chg_backup_soc(int backup_soc);
extern int oplus_sm8150_get_pd_type(void);
extern int oplus_chg_set_pd_config(void);
extern void oplus_get_usbtemp_volt(struct oplus_chg_chip *chip);
extern void oplus_set_typec_sinkonly(void);
extern void oplus_set_typec_cc_open(void);
extern void oplus_start_usb_peripheral(void);
extern int qpnp_get_prop_charger_voltage_now(void);
#endif
extern bool oplus_pd_without_usb(void);
extern void oplus_wake_up_usbtemp_thread(void);
extern bool oplus_chg_wake_update_work(void);
extern int get_boot_mode(void);
extern bool oplus_check_pdphy_ready(void);
extern int oplus_chg_get_curr_time_ms(unsigned long *time_ms);
extern bool oplus_pd_connected(void);

#define DEFAULT_CV 4435
#define SY6970_HVDCP_BC12_WORK_DELAY 1500


#define OPLUS_BC12_RETRY_TIME             round_jiffies_relative(msecs_to_jiffies(200))
#define OPLUS_BC12_RETRY_TIME_CDP         round_jiffies_relative(msecs_to_jiffies(400))
#define BQ_CHARGER_CURRENT_MAX_MA         (3400)

#define SY6970_ERR   (1 << 0)
#define SY6970_INFO  (1 << 1)
#define SY6970_DEBUG (1 << 2)
#define INIT_WORK_NORMAL_DELAY 6000
#define INIT_WORK_OTHER_DELAY 1000
#define AICL_POINT_VOL_9V 7600
#define AICL_POINT_VOL_5V 4140
#define HW_AICL_POINT_VOL_5V_PHASE1 4440
#define HW_AICL_POINT_VOL_5V_PHASE2 4520
#define SW_AICL_POINT_VOL_5V_PHASE1 4500
#define SW_AICL_POINT_VOL_5V_PHASE2 4535
#define UNIT_TRANS_1000 1000

static int sy6970_chg_dbg_enable = SY6970_ERR|SY6970_INFO|SY6970_DEBUG;
module_param(sy6970_chg_dbg_enable, int, 0644);
MODULE_PARM_DESC(sy6970_chg_dbg_enable, "debug charger sy6970");

#ifdef chg_debug
#undef chg_debug
#define chg_debug(fmt, ...) \
	if (sy6970_chg_dbg_enable & SY6970_DEBUG ) { \
		printk(KERN_ERR "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
	} else { \
		printk(KERN_NOTICE "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
	}
#endif

#ifdef chg_info
#undef chg_info
#define chg_info(fmt, ...) \
	if (sy6970_chg_dbg_enable & SY6970_INFO) { \
		printk(KERN_ERR "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
	} else { \
		printk(KERN_NOTICE "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
	}
#else
#define chg_info(fmt, ...) \
	if (sy6970_chg_dbg_enable & SY6970_INFO) { \
                printk(KERN_ERR "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
        } else { \
                printk(KERN_NOTICE "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
        }
#endif

#ifdef chg_err
#undef chg_err
#define chg_err(fmt, ...) \
	if (sy6970_chg_dbg_enable & SY6970_ERR) { \
		printk(KERN_ERR "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
	} else { \
		printk(KERN_NOTICE "[OPLUS_CHG][%s]"fmt, __func__, ##__VA_ARGS__); \
	}
#endif

enum charger_type {
	CHARGER_UNKNOWN = 0,
	STANDARD_HOST,      /* USB : 450mA */
	CHARGING_HOST,
	NONSTANDARD_CHARGER,    /* AC : 450mA~1A */
	STANDARD_CHARGER,   /* AC : ~1A */
	APPLE_2_1A_CHARGER, /* 2.1A apple charger */
	APPLE_1_0A_CHARGER, /* 1A apple charger */
	APPLE_0_5A_CHARGER, /* 0.5A apple charger */
	WIRELESS_CHARGER,
};

enum {
	PN_BQ25890H,
	PN_BQ25892,
	PN_BQ25895,
};

enum hvdcp_type {
	HVDCP_5V,
	HVDCP_9V,
	HVDCP_12V,
	HVDCP_20V,
	HVDCP_CONTINOUS,
	HVDCP_DPF_DMF,
};

struct chg_para{
	int vlim;
	int ilim;
	int vreg;
	int ichg;
};

struct sy6970_platform_data {
	int iprechg;
	int iterm;
	int boostv;
	int boosti;
	struct chg_para usb;
};

struct sy6970 {
	struct device *dev;
	struct i2c_client *client;
	struct delayed_work sy6970_aicr_setting_work;
	struct delayed_work sy6970_retry_adapter_detection;
	struct delayed_work sy6970_vol_convert_work;
	struct delayed_work init_work;
	/*struct delayed_work enter_hz_work;*/
	struct delayed_work sy6970_hvdcp_bc12_work;
#ifdef CONFIG_TCPC_CLASS
	/*type_c_port0*/
	struct tcpc_device *tcpc;
#endif
	int part_no;
	int revision;

	const char *chg_dev_name;
	const char *eint_name;

	atomic_t   charger_suspended;
	atomic_t   is_suspended;
	bool chg_det_enable;
	bool otg_enable;

	enum charger_type chg_type;
	enum power_supply_type oplus_chg_type;

	int status;
	int irq;

	int irq_gpio;
	struct pinctrl *pinctrl;
	struct pinctrl_state *splitchg_inter_active;
	struct pinctrl_state *splitchg_inter_sleep;

	struct mutex i2c_rw_lock;
	struct mutex chgdet_en_lock;

	bool charge_enabled;	/* Register bit status */
	bool power_good;
	bool chg_need_check;
	struct sy6970_platform_data *platform_data;
	struct charger_device *chg_dev;
	struct timespec ptime[2];

	struct power_supply *psy;
#ifdef CONFIG_OPLUS_CHARGER_MTK
	struct charger_consumer *chg_consumer;
#endif
	bool disable_hight_vbus;
	bool pdqc_setup_5v;
	bool is_bc12_end;
	bool is_force_aicl;
	bool hvdcp_can_enabled;
	int pre_current_ma;
	int pre_cur;
	int aicr;
	int chg_cur;
	int vbus_type;
	bool hvdcp_checked;
	int hw_aicl_point;
	bool retry_hvdcp_algo;
	bool nonstand_retry_bc;
	bool camera_on;
	bool calling_on;
	bool is_sy6970;

	bool is_force_dpdm;
	bool cdp_retry;
	bool sdp_retry;
	bool chg_start_check;
	bool is_retry_bc12;
	bool cdp_retry_aicl;
	bool usb_connect_start;
	int boot_mode;
	int  qc_to_9v_count;

	struct mutex		dpdm_lock;
	struct regulator	*dpdm_reg;
	bool			dpdm_enabled;
	bool qc_aicl_true;
};

static bool disable_PE = 0;
static bool disable_QC = 0;
static bool disable_PD = 0;
static bool dumpreg_by_irq = 1;
static int  current_percent = 50;

module_param(disable_PE, bool, 0644);
module_param(disable_QC, bool, 0644);
module_param(disable_PD, bool, 0644);
module_param(current_percent, int, 0644);
module_param(dumpreg_by_irq, bool, 0644);


static struct sy6970 *g_bq;
struct task_struct *charger_type_kthread;
static DECLARE_WAIT_QUEUE_HEAD(oplus_chgtype_wq);
extern struct oplus_chg_chip *g_oplus_chip;

#ifdef CONFIG_OPLUS_CHARGER_MTK
extern void Charger_Detect_Init(void);
extern void Charger_Detect_Release(void);

#else	/*CONFIG_OPLUS_CHARGER_MTK*/
#define META_BOOT	0
#endif

void oplus_sy6970_set_mivr_by_battery_vol(void);
static void sy6970_dump_regs(struct sy6970 *bq);
int sy6970_enable_enlim(struct sy6970 *bq);
int oplus_sy6970_set_ichg(int cur);
static bool sy6970_is_dcp(struct sy6970 *bq);
static bool sy6970_is_hvdcp(struct sy6970 *bq);
static bool sy6970_is_usb(struct sy6970 *bq);
int oplus_sy6970_get_vbus(void);
static int oplus_sy6970_get_pd_type(void);
static int oplus_sy6970_charger_suspend(void);

#ifdef CONFIG_OPLUS_CHARGER_MTK
static const struct charger_properties sy6970_chg_props = {
	.alias_name = "sy6970",
};
#endif

#ifndef CONFIG_OPLUS_CHARGER_MTK
static int sy6970_request_dpdm(struct sy6970 *chip, bool enable);
void Charger_Detect_Init(void)
{
	chg_info(" enter\n");
        if (g_bq) {
                sy6970_request_dpdm(g_bq, true);
        }
}
void Charger_Detect_Release(void)
{
        if (g_bq) {
                sy6970_request_dpdm(g_bq, false);
        }
}
#endif /*CONFIG_OPLUS_CHARGER_MTK*/

void oplus_for_cdp(void)
{
	chg_debug("usb_rdy PASS");
}

#define I2C_RETRY_DELAY_US	5000
#define I2C_RETRY_MAX_COUNT	3
static int g_sy6970_read_reg(struct sy6970 *bq, u8 reg, u8 *data)
{
	s32 ret = 0;
	int retry = I2C_RETRY_MAX_COUNT;

	ret = i2c_smbus_read_byte_data(bq->client, reg);
	if (ret < 0) {
		while(retry > 0) {
			usleep_range(I2C_RETRY_DELAY_US, I2C_RETRY_DELAY_US);
			ret = i2c_smbus_read_byte_data(bq->client, reg);
			if (ret < 0) {
				retry--;
			} else {
				break;
			}
		}
	}

	if (ret < 0) {
		pr_err("i2c read fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*data = (u8) ret;

	return 0;
}

static int g_sy6970_write_reg(struct sy6970 *bq, int reg, u8 val)
{
	s32 ret;
	int retry_cnt = I2C_RETRY_MAX_COUNT;

	ret = i2c_smbus_write_byte_data(bq->client, reg, val);

	if (ret < 0) {
		while(retry_cnt > 0) {
			usleep_range(I2C_RETRY_DELAY_US, I2C_RETRY_DELAY_US);
			ret = i2c_smbus_write_byte_data(bq->client, reg, val);
			if (ret < 0) {
				retry_cnt--;
			} else {
				break;
			}
		}
	}

	if (ret < 0) {
		pr_err("i2c write fail: can't write 0x%02X to reg 0x%02X: %d\n",
		       val, reg, ret);
		return ret;
	}
	return 0;
}

static int sy6970_read_byte(struct sy6970 *bq, u8 reg, u8 *data)
{
	int ret;

	mutex_lock(&bq->i2c_rw_lock);
	ret = g_sy6970_read_reg(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	return ret;
}

static int sy6970_write_byte(struct sy6970 *bq, u8 reg, u8 data)
{
	int ret;

	mutex_lock(&bq->i2c_rw_lock);
	ret = g_sy6970_write_reg(bq, reg, data);
	mutex_unlock(&bq->i2c_rw_lock);

	if (ret)
		pr_err("Failed: reg=%02X, ret=%d\n", reg, ret);

	return ret;
}

static int sy6970_update_bits(struct sy6970 *bq, u8 reg, u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	mutex_lock(&bq->i2c_rw_lock);
	ret = g_sy6970_read_reg(bq, reg, &tmp);
	if (ret) {
		pr_err("Failed: reg=%02X, ret=%d\n", reg, ret);
		goto out;
	}

	tmp &= ~mask;
	tmp |= data & mask;

	ret = g_sy6970_write_reg(bq, reg, tmp);
	if (ret)
		pr_err("Failed: reg=%02X, ret=%d\n", reg, ret);

out:
	mutex_unlock(&bq->i2c_rw_lock);
	return ret;
}

#define SUB_CHG_SUSPEND_DELAY_MS	50
static int sy6970_enable_otg(struct sy6970 *bq)
{
	u8 val = SY6970_OTG_ENABLE << SY6970_OTG_CONFIG_SHIFT;

	sy6970_update_bits(bq, SY6970_REG_03,
				   SY6970_OTG_CONFIG_MASK, val);

	if (g_oplus_chip->is_double_charger_support) {
		msleep(SUB_CHG_SUSPEND_DELAY_MS);
		g_oplus_chip->sub_chg_ops->charger_suspend();
	}

	return 0;
}

static int sy6970_disable_otg(struct sy6970 *bq)
{
	u8 val = SY6970_OTG_DISABLE << SY6970_OTG_CONFIG_SHIFT;

	return sy6970_update_bits(bq, SY6970_REG_03,
				   SY6970_OTG_CONFIG_MASK, val);
}

static int sy6970_enable_hvdcp(struct sy6970 *bq)
{
	int ret;
	u8 val = SY6970_HVDCP_ENABLE << SY6970_HVDCPEN_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_02,
				SY6970_HVDCPEN_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_enable_hvdcp);

static int sy6970_disable_hvdcp(struct sy6970 *bq)
{
	int ret;
	u8 val = SY6970_HVDCP_DISABLE << SY6970_HVDCPEN_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_02,
				SY6970_HVDCPEN_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_disable_hvdcp);

static int sy6970_disable_maxc(struct sy6970 *bq)
{
	int ret;
	u8 val = SY6970_MAXC_DISABLE << SY6970_MAXCEN_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_02,
				SY6970_MAXCEN_MASK, val);
	return ret;
}

static int sy6970_disable_batfet_rst(struct sy6970 *bq)
{
	int ret;
	u8 val = SY6970_BATFET_RST_EN_DISABLE << SY6970_BATFET_RST_EN_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_09,
				SY6970_BATFET_RST_EN_MASK, val);
	return ret;
}

static int sy6970_enable_charger(struct sy6970 *bq)
{
	int ret = 0;
	u8 val = SY6970_CHG_ENABLE << SY6970_CHG_CONFIG_SHIFT;

	if (!bq) {
		return 0;
	}

	chg_info(" enable \n");
	if (atomic_read(&bq->charger_suspended) == 1) {
		chg_err("suspend, ignore\n");
		return 0;
	}

	ret = sy6970_update_bits(bq, SY6970_REG_03,
				SY6970_CHG_CONFIG_MASK, val);

	if ((g_oplus_chip != NULL)
		&& (g_oplus_chip->is_double_charger_support)
		&& g_oplus_chip->slave_charger_enable) {
		if (!sy6970_is_usb(bq)) {
			chg_debug("enable slave charger.\n");
			if (!g_oplus_chip->sub_chg_ops) {
				ret = g_oplus_chip->sub_chg_ops->charging_enable();
			}
		} else {
			chg_debug("disable slave charger.\n");
			if (!g_oplus_chip->sub_chg_ops) {
				ret = g_oplus_chip->sub_chg_ops->charging_disable();
			}
		}
	}

	return ret;
}

static void oplus_sy6970_subcharger_force_enable(void)
{
	if (g_oplus_chip && g_oplus_chip->sub_chg_ops) {
		g_oplus_chip->sub_chg_ops->charging_enable();
	}
}

static int sy6970_disable_charger(struct sy6970 *bq)
{
	int ret = 0;
	u8 val = SY6970_CHG_DISABLE << SY6970_CHG_CONFIG_SHIFT;

	chg_info("disable \n");
	ret = sy6970_update_bits(bq, SY6970_REG_03,
				SY6970_CHG_CONFIG_MASK, val);

	if ((g_oplus_chip != NULL) && (g_oplus_chip->is_double_charger_support)) {
		g_oplus_chip->sub_chg_ops->charging_disable();
	}

	return ret;
}


static int sy6970_adc_start_one_shot(struct sy6970 *bq)
{
        u8 val = 0;
        int ret = 0;

        ret = sy6970_read_byte(bq, SY6970_REG_02, &val);
        if (ret < 0) {
                dev_err(bq->dev, "%s failed to read register 0x02:%d\n", __func__, ret);
                return ret;
        }



        ret = sy6970_update_bits(bq, SY6970_REG_02, SY6970_CONV_START_MASK,
                                        SY6970_CONV_START << SY6970_CONV_START_SHIFT);
        if (ret < 0) {
                dev_err(bq->dev, "%s adc start failed, ret=%d\n", __func__, ret);
        }

        return ret;
}

int sy6970_adc_start(struct sy6970 *bq)
{
	u8 val = 0;
	int ret = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_02, &val);
	if (ret < 0) {
		dev_err(bq->dev, "%s failed to read register 0x02:%d\n", __func__, ret);
		return ret;
	}

	if (((val & SY6970_CONV_RATE_MASK) >> SY6970_CONV_RATE_SHIFT) == SY6970_ADC_CONTINUE_ENABLE) {
		return 0; /*is doing continuous scan*/
	}


	ret = sy6970_update_bits(bq, SY6970_REG_02, SY6970_CONV_RATE_MASK,
					SY6970_ADC_CONTINUE_ENABLE << SY6970_CONV_RATE_SHIFT);
	if (ret < 0) {
		dev_err(bq->dev, "%s enable continue adc convert failed, ret=%d\n", __func__, ret);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_adc_start);

int sy6970_adc_stop(struct sy6970 *bq)
{
	return sy6970_update_bits(bq, SY6970_REG_02, SY6970_CONV_RATE_MASK,
				SY6970_ADC_CONTINUE_DISABLE << SY6970_CONV_RATE_SHIFT);
}
EXPORT_SYMBOL_GPL(sy6970_adc_stop);


int sy6970_adc_read_battery_volt(struct sy6970 *bq)
{
	uint8_t val = 0;
	int volt = 0;
	int ret = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_0E, &val);
	if (ret < 0) {
		dev_err(bq->dev, "read battery voltage failed :%d\n", ret);
		return ret;
	} else {
		volt = SY6970_BATV_BASE + ((val & SY6970_BATV_MASK) >> SY6970_BATV_SHIFT) * SY6970_BATV_LSB;
		return volt;
	}
}
EXPORT_SYMBOL_GPL(sy6970_adc_read_battery_volt);


int sy6970_adc_read_sys_volt(struct sy6970 *bq)
{
	uint8_t val;
	int volt;
	int ret;
	ret = sy6970_read_byte(bq, SY6970_REG_0F, &val);
	if (ret < 0) {
		dev_err(bq->dev, "read system voltage failed :%d\n", ret);
		return ret;
	} else {
		volt = SY6970_SYSV_BASE + ((val & SY6970_SYSV_MASK) >> SY6970_SYSV_SHIFT) * SY6970_SYSV_LSB;
		return volt;
	}
}
EXPORT_SYMBOL_GPL(sy6970_adc_read_sys_volt);

static int _sy6970_adc_read_vbus_volt(struct sy6970 *sy) {
	uint8_t val = 0;
        int volt = 0;
        int ret = 0;

	ret = sy6970_read_byte(sy, SY6970_REG_11, &val);
        if (ret < 0) {
                dev_err(sy->dev, "read vbus voltage failed :%d\n", ret);
                return ret;
        } else {
                volt = SY6970_VBUSV_BASE + ((val & SY6970_VBUSV_MASK) >> SY6970_VBUSV_SHIFT) * SY6970_VBUSV_LSB;
                return volt;
        }
}

#define SY6970_INVALID_VBUS_MV	2600
#define SY6970_VBUS_RETRY_MS	10
int sy6970_adc_read_vbus_volt(struct sy6970 *sy)
{
	int ret = 0;
	int retry = 20;

	/*Note: 1. When do BC1.2, vbus can be read after the BC1.2 is completed (300-500ms after start fore dpdm);
	 2. When not do BC1.2, vbus can be read after IC ready (the IC can work after 500 ms of power on).*/
	if (sy->power_good) {
		while (retry > 0) {
			sy6970_adc_start_one_shot(sy);
			ret = _sy6970_adc_read_vbus_volt(sy);
			if ((ret >= 0) && (ret != SY6970_INVALID_VBUS_MV)) {
				break;
			} else {
				/* chg_debug("get the vbus = %d mV is invalid.\n", ret);*/
				msleep(SY6970_VBUS_RETRY_MS);
				retry--;
			}
		}

		if ((retry <= 0) && (ret == SY6970_INVALID_VBUS_MV)) {
			chg_err("get the vbus result= %d mV is invalid.\n", ret);
		}
	} else {
		sy6970_adc_start_one_shot(sy);
		ret = _sy6970_adc_read_vbus_volt(sy);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_adc_read_vbus_volt);

int sy6970_adc_read_temperature(struct sy6970 *sy)
{
	uint8_t val = 0;
	int temp = 0;
	int ret = 0;

	ret = sy6970_read_byte(sy, SY6970_REG_10, &val);
	if (ret < 0) {
		dev_err(sy->dev, "read temperature failed :%d\n", ret);
		return ret;
	} else {
		temp = SY6970_TSPCT_BASE + ((val & SY6970_TSPCT_MASK) >> SY6970_TSPCT_SHIFT) * SY6970_TSPCT_LSB;
		return temp;
	}
}
EXPORT_SYMBOL_GPL(sy6970_adc_read_temperature);

int sy6970_adc_read_charge_current(void)
{
	uint8_t val = 0;
	int volt = 0;
	int ret = 0;

	ret = sy6970_read_byte(g_bq, SY6970_REG_12, &val);
	if (ret < 0) {
		dev_err(g_bq->dev, "read charge current failed :%d\n", ret);
		return ret;
	} else{
		volt = (int)(SY6970_ICHGR_BASE + ((val & SY6970_ICHGR_MASK) >> SY6970_ICHGR_SHIFT) * SY6970_ICHGR_LSB);
		return volt;
	}
}
EXPORT_SYMBOL_GPL(sy6970_adc_read_charge_current);
int sy6970_set_chargecurrent(struct sy6970 *sy, int curr)
{
	u8 ichg = 0;

	chg_info(" ichg = %d\n", curr);

	if (curr < SY6970_ICHG_BASE)
		curr = SY6970_ICHG_BASE;

	ichg = (curr - SY6970_ICHG_BASE)/SY6970_ICHG_LSB;
	return sy6970_update_bits(sy, SY6970_REG_04,
						SY6970_ICHG_MASK, ichg << SY6970_ICHG_SHIFT);
}

int sy6970_set_term_current(struct sy6970 *sy, int curr)
{
	u8 iterm = 0;

	if ((g_oplus_chip != NULL) && (g_oplus_chip->is_double_charger_support)) {
		g_oplus_chip->sub_chg_ops->term_current_set(curr + 200);
	}

	if (curr < SY6970_ITERM_BASE)
		curr = SY6970_ITERM_BASE;

	iterm = (curr - SY6970_ITERM_BASE) / SY6970_ITERM_LSB;

	return sy6970_update_bits(sy, SY6970_REG_05,
						SY6970_ITERM_MASK, iterm << SY6970_ITERM_SHIFT);
}
EXPORT_SYMBOL_GPL(sy6970_set_term_current);

void sy6970_really_suspend_charger(bool en)
{
	int rc = 0;

	if (!g_bq) {
		return;
	}

	if (atomic_read(&g_bq->charger_suspended) == 1) {
		return;
	}

	rc = sy6970_update_bits(g_bq, SY6970_REG_00, SY6970_ENHIZ_MASK,
	en ? (SY6970_HIZ_ENABLE << SY6970_ENHIZ_SHIFT) : (SY6970_HIZ_DISABLE << SY6970_ENHIZ_SHIFT));

	if (rc < 0) {
		chg_err("fail en=%d rc = %d\n", en, rc);
	}
}

int sy6970_set_prechg_current(struct sy6970 *sy, int curr)
{
	u8 iprechg = 0;

	if (curr < SY6970_IPRECHG_BASE)
		curr = SY6970_IPRECHG_BASE;

	iprechg = (curr - SY6970_IPRECHG_BASE) / SY6970_IPRECHG_LSB;

	return sy6970_update_bits(sy, SY6970_REG_05,
						SY6970_IPRECHG_MASK, iprechg << SY6970_IPRECHG_SHIFT);
}
EXPORT_SYMBOL_GPL(sy6970_set_prechg_current);

int sy6970_set_chargevolt(struct sy6970 *sy, int volt)
{
	u8 val = 0;

	chg_debug("volt = %d", volt);

	if ((g_oplus_chip != NULL) && (g_oplus_chip->is_double_charger_support)) {
		g_oplus_chip->sub_chg_ops->float_voltage_write(volt);
	}

	if (volt < SY6970_VREG_BASE)
		volt = SY6970_VREG_BASE;

	val = (volt - SY6970_VREG_BASE)/SY6970_VREG_LSB;
	return sy6970_update_bits(sy, SY6970_REG_06,
						SY6970_VREG_MASK, val << SY6970_VREG_SHIFT);
}

int sy6970_set_input_volt_limit(struct sy6970 *sy, int volt)
{
	u8 val = 0;

	chg_debug(" volt = %d", volt);

	if (volt < SY6970_VINDPM_BASE)
		volt = SY6970_VINDPM_BASE;

	val = (volt - SY6970_VINDPM_BASE) / SY6970_VINDPM_LSB;

	sy6970_update_bits(sy, SY6970_REG_0D,
						SY6970_FORCE_VINDPM_MASK, SY6970_FORCE_VINDPM_ENABLE << SY6970_FORCE_VINDPM_SHIFT);

	return sy6970_update_bits(sy, SY6970_REG_0D,
						SY6970_VINDPM_MASK, val << SY6970_VINDPM_SHIFT);
}

int sy6970_set_input_current_limit(struct sy6970 *sy, int curr)
{
	u8 val = 0;

	chg_debug("curr = %d", curr);

	if (curr < SY6970_IINLIM_BASE)
		curr = SY6970_IINLIM_BASE;

	val = (curr - SY6970_IINLIM_BASE) / SY6970_IINLIM_LSB;

	return sy6970_update_bits(sy, SY6970_REG_00, SY6970_IINLIM_MASK,
						val << SY6970_IINLIM_SHIFT);
}


int sy6970_set_watchdog_timer(struct sy6970 *sy, u8 timeout)
{
	u8 val = 0;

	val = (timeout - SY6970_WDT_BASE) / SY6970_WDT_LSB;
	val <<= SY6970_WDT_SHIFT;

	return sy6970_update_bits(sy, SY6970_REG_07,
						SY6970_WDT_MASK, val);
}
EXPORT_SYMBOL_GPL(sy6970_set_watchdog_timer);

int sy6970_disable_watchdog_timer(struct sy6970 *sy)
{
	u8 val = SY6970_WDT_DISABLE << SY6970_WDT_SHIFT;

	return sy6970_update_bits(sy, SY6970_REG_07,
						SY6970_WDT_MASK, val);
}
EXPORT_SYMBOL_GPL(sy6970_disable_watchdog_timer);

int sy6970_reset_watchdog_timer(struct sy6970 *sy)
{
	u8 val = SY6970_WDT_RESET << SY6970_WDT_RESET_SHIFT;

	return sy6970_update_bits(sy, SY6970_REG_03,
						SY6970_WDT_RESET_MASK, val);
}
EXPORT_SYMBOL_GPL(sy6970_reset_watchdog_timer);


int sy6970_force_dpdm(struct sy6970 *sy, bool enable)
{
	int ret = 0;
	u8 val = 0;

	if (enable) {
		val = SY6970_AUTO_DPDM_ENABLE << SY6970_FORCE_DPDM_SHIFT;
	} else {
		val = SY6970_AUTO_DPDM_DISABLE << SY6970_FORCE_DPDM_SHIFT;
	}

	ret = sy6970_update_bits(sy, SY6970_REG_02, SY6970_FORCE_DPDM_MASK, val);

	chg_info("Force DPDM %s, enable=%d\n", !ret ?  "successfully" : "failed", enable);
	return ret;

}
EXPORT_SYMBOL_GPL(sy6970_force_dpdm);

int sy6970_reset_chip(struct sy6970 *sy)
{
	int ret = 0;
	u8 val = SY6970_RESET << SY6970_RESET_SHIFT;

	ret = sy6970_update_bits(sy, SY6970_REG_14,
						SY6970_RESET_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_reset_chip);

int sy6970_enable_enlim(struct sy6970 *sy)
{
	u8 val = SY6970_ENILIM_ENABLE << SY6970_ENILIM_SHIFT;
	return sy6970_update_bits(sy, SY6970_REG_00, SY6970_ENILIM_MASK, val);
}
EXPORT_SYMBOL_GPL(sy6970_enable_enlim);

int sy6970_enter_hiz_mode(struct sy6970 *sy)
{
	u8 val = 0;
	int result = 0;
	int boot_mode = get_boot_mode();

	chg_info(" enter");
#ifdef CONFIG_OPLUS_CHARGER_MTK
	if ((boot_mode == META_BOOT)
		|| (g_oplus_chip->mmi_chg == 0)) {
		/*val = SY6970_HIZ_ENABLE << SY6970_ENHIZ_SHIFT;*/
		val = SY6970_HIZ_DISABLE << SY6970_ENHIZ_SHIFT;
		result = sy6970_update_bits(sy, SY6970_REG_00, SY6970_ENHIZ_MASK, val);
		result = sy6970_disable_charger(sy);
		sy6970_set_input_current_limit(sy, 0);
	} else {
		result = sy6970_disable_charger(sy);
	}
#else
	if (boot_mode == MSM_BOOT_MODE__RF || boot_mode == MSM_BOOT_MODE__WLAN) {
		/*val = SY6970_HIZ_ENABLE << SY6970_ENHIZ_SHIFT;*/
		val = SY6970_HIZ_DISABLE << SY6970_ENHIZ_SHIFT;
		result = sy6970_update_bits(sy, SY6970_REG_00, SY6970_ENHIZ_MASK, val);
		result = sy6970_disable_charger(sy);
		sy6970_set_input_current_limit(sy, 0);
	} else {
		result = sy6970_disable_charger(sy);
	}
#endif
	return result;
}
EXPORT_SYMBOL_GPL(sy6970_enter_hiz_mode);

int sy6970_exit_hiz_mode(struct sy6970 *sy)
{
	u8 val = 0;

	chg_info(" enter");

	val = SY6970_HIZ_DISABLE << SY6970_ENHIZ_SHIFT;
	sy6970_update_bits(sy, SY6970_REG_00, SY6970_ENHIZ_MASK, val);

	return 0;
}
EXPORT_SYMBOL_GPL(sy6970_exit_hiz_mode);

int sy6970_disable_enlim(struct sy6970 *sy)
{
	u8 val = SY6970_ENILIM_DISABLE << SY6970_ENILIM_SHIFT;

	return sy6970_update_bits(sy, SY6970_REG_00,
						SY6970_ENILIM_MASK, val);

}
EXPORT_SYMBOL_GPL(sy6970_disable_enlim);

int sy6970_get_hiz_mode(struct sy6970 *bq, u8 *state)
{
	u8 val = 0;
	int ret = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_00, &val);
	if (ret)
		return ret;
	*state = (val & SY6970_ENHIZ_MASK) >> SY6970_ENHIZ_SHIFT;

	return 0;
}
EXPORT_SYMBOL_GPL(sy6970_get_hiz_mode);

static int sy6970_enable_term(struct sy6970 *bq, bool enable)
{
	u8 val = 0;
	int ret = 0;

	if (enable)
		val = SY6970_TERM_ENABLE << SY6970_EN_TERM_SHIFT;
	else
		val = SY6970_TERM_DISABLE << SY6970_EN_TERM_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_07,
						SY6970_EN_TERM_MASK, val);
	if ((g_oplus_chip != NULL)
		&& (g_oplus_chip->is_double_charger_support)) {
		g_oplus_chip->sub_chg_ops->set_charging_term_disable();
	}

	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_enable_term);

int sy6970_set_boost_current(struct sy6970 *bq, int curr)
{
	u8 temp = 0;

	if (curr < 750)
		temp = SY6970_BOOST_LIM_500MA;
	else if (curr < 1200)
		temp = SY6970_BOOST_LIM_750MA;
	else if (curr < 1400)
		temp = SY6970_BOOST_LIM_1200MA;
	else if (curr < 1650)
		temp = SY6970_BOOST_LIM_1400MA;
	else if (curr < 1870)
		temp = SY6970_BOOST_LIM_1650MA;
	else if (curr < 2150)
		temp = SY6970_BOOST_LIM_1875MA;
	else if (curr < 2450)
		temp = SY6970_BOOST_LIM_2150MA;
	else
		temp= SY6970_BOOST_LIM_2450MA;

	chg_info("boost current temp = %d mA",temp);
	return sy6970_update_bits(bq, SY6970_REG_0A,
				SY6970_BOOST_LIM_MASK,
				temp << SY6970_BOOST_LIM_SHIFT);
}

static int sy6970_vmin_limit(struct sy6970 *bq)
{
        u8 val = 4 << SY6970_SYS_MINV_SHIFT;

        return sy6970_update_bits(bq, SY6970_REG_03,
                                   SY6970_SYS_MINV_MASK, val);
}

static int sy6970_enable_auto_dpdm(struct sy6970* bq, bool enable)
{
	u8 val = 0;
	int ret = 0;

	if (enable)
		val = SY6970_AUTO_DPDM_ENABLE << SY6970_AUTO_DPDM_EN_SHIFT;
	else
		val = SY6970_AUTO_DPDM_DISABLE << SY6970_AUTO_DPDM_EN_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_02,
						SY6970_AUTO_DPDM_EN_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_enable_auto_dpdm);

int sy6970_set_boost_voltage(struct sy6970 *bq, int volt)
{
	u8 val = 0;

	if (volt < SY6970_BOOSTV_BASE)
		volt = SY6970_BOOSTV_BASE;
	if (volt > SY6970_BOOSTV_BASE
			+ (SY6970_BOOSTV_MASK >> SY6970_BOOSTV_SHIFT)
			* SY6970_BOOSTV_LSB)
		volt = SY6970_BOOSTV_BASE
			+ (SY6970_BOOSTV_MASK >> SY6970_BOOSTV_SHIFT)
			* SY6970_BOOSTV_LSB;

	val = ((volt - SY6970_BOOSTV_BASE) / SY6970_BOOSTV_LSB)
			<< SY6970_BOOSTV_SHIFT;

	return sy6970_update_bits(bq, SY6970_REG_0A,
				SY6970_BOOSTV_MASK, val);
}
EXPORT_SYMBOL_GPL(sy6970_set_boost_voltage);

static int sy6970_enable_ico(struct sy6970* bq, bool enable)
{
	u8 val = 0;
	int ret = 0;

	if (enable)
		val = SY6970_ICO_ENABLE << SY6970_ICOEN_SHIFT;
	else
		val = SY6970_ICO_DISABLE << SY6970_ICOEN_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_02, SY6970_ICOEN_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sy6970_enable_ico);

static int sy6970_read_idpm_limit(struct sy6970 *bq, int *icl)
{
	uint8_t val = 0;
	int ret = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_13, &val);
	if (ret < 0) {
		dev_err(bq->dev, "read vbus voltage failed :%d\n", ret);
		return ret;
	} else{
		*icl = SY6970_IDPM_LIM_BASE + ((val & SY6970_IDPM_LIM_MASK) >> SY6970_IDPM_LIM_SHIFT) * SY6970_IDPM_LIM_LSB ;
		return 0;
	}
}
EXPORT_SYMBOL_GPL(sy6970_read_idpm_limit);

static int sy6970_enable_safety_timer(struct sy6970 *bq)
{
	const u8 val = SY6970_CHG_TIMER_ENABLE << SY6970_EN_TIMER_SHIFT;

	return sy6970_update_bits(bq, SY6970_REG_07, SY6970_EN_TIMER_MASK,
				   val);
}
EXPORT_SYMBOL_GPL(sy6970_enable_safety_timer);

static int sy6970_disable_safety_timer(struct sy6970 *bq)
{
	const u8 val = SY6970_CHG_TIMER_DISABLE << SY6970_EN_TIMER_SHIFT;

	return sy6970_update_bits(bq, SY6970_REG_07, SY6970_EN_TIMER_MASK, val);
}
EXPORT_SYMBOL_GPL(sy6970_disable_safety_timer);


static int sy6970_switch_to_hvdcp(struct sy6970 *bq, enum hvdcp_type type)
{
	int ret = 0;
	int val = 0;

	switch (type) {
		case HVDCP_5V:
			val = SY6970_HVDCP_DISABLE << SY6970_HVDCPEN_SHIFT;
			ret = sy6970_update_bits(bq, SY6970_REG_02, 8, val);
			Charger_Detect_Init();
			oplus_for_cdp();
			sy6970_force_dpdm(bq, true);
			bq->qc_aicl_true = true;
			chg_info(" set to 5v\n");
			break;

		case HVDCP_9V:
			/*HV_TYPE: Higher Voltage Types*/
			val = SY6970_HVDCP_DISABLE << SY6970_HVDCPHV_SHIFT;
			ret = sy6970_update_bits(bq, SY6970_REG_02, 4, val);
			bq->qc_aicl_true = true;
			chg_info(" set to 9v\n");
			break;

		default:
			chg_err(" not support now.\n");
			break;
	}
	return ret;
}

static int sy6970_check_charge_done(struct sy6970 *bq, bool *done)
{
	int ret = 0;
	u8 val = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_0B, &val);
	if (!ret) {
		val = val & SY6970_CHRG_STAT_MASK;
		val = val >> SY6970_CHRG_STAT_SHIFT;
		*done = (val == SY6970_CHRG_STAT_CHGDONE);
	}

	return ret;
}

static struct sy6970_platform_data *sy6970_parse_dt(struct device_node *np,
						      struct sy6970 *bq)
{
	int ret = 0;
	struct sy6970_platform_data *pdata;

	pdata = devm_kzalloc(bq->dev, sizeof(struct sy6970_platform_data),
			     GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (of_property_read_string(np, "charger_name", &bq->chg_dev_name) < 0) {
		bq->chg_dev_name = "primary_chg";
		pr_warn("no charger name\n");
	}

	if (of_property_read_string(np, "eint_name", &bq->eint_name) < 0) {
		bq->eint_name = "chr_stat";
		pr_warn("no eint name\n");
	}

	bq->chg_det_enable =
	    of_property_read_bool(np, "ti,sy6970,charge-detect-enable");

	ret = of_property_read_u32(np, "ti,sy6970,usb-vlim", &pdata->usb.vlim);
	if (ret) {
		pdata->usb.vlim = 4500;
		pr_err("Failed to read node of ti,sy6970,usb-vlim\n");
	}

	ret = of_property_read_u32(np, "ti,sy6970,usb-ilim", &pdata->usb.ilim);
	if (ret) {
		pdata->usb.ilim = 2000;
		pr_err("Failed to read node of ti,sy6970,usb-ilim\n");
	}

	ret = of_property_read_u32(np, "ti,sy6970,usb-vreg", &pdata->usb.vreg);
	if (ret) {
		pdata->usb.vreg = 4200;
		pr_err("Failed to read node of ti,sy6970,usb-vreg\n");
	}

	ret = of_property_read_u32(np, "ti,sy6970,usb-ichg", &pdata->usb.ichg);
	if (ret) {
		pdata->usb.ichg = 2000;
		pr_err("Failed to read node of ti,sy6970,usb-ichg\n");
	}

	ret = of_property_read_u32(np, "ti,sy6970,precharge-current",
				   &pdata->iprechg);
	if (ret) {
		pdata->iprechg = 256;
		pr_err("Failed to read node of ti,sy6970,precharge-current\n");
	}

	ret = of_property_read_u32(np, "ti,sy6970,termination-current",
				   &pdata->iterm);
	if (ret) {
		pdata->iterm = 250;
		pr_err("Failed to read node of ti,sy6970,termination-current\n");
	}

	ret =
	    of_property_read_u32(np, "ti,sy6970,boost-voltage",
				 &pdata->boostv);
	if (ret) {
		pdata->boostv = 5000;
		pr_err("Failed to read node of ti,sy6970,boost-voltage\n");
	}

	ret =
	    of_property_read_u32(np, "ti,sy6970,boost-current",
				 &pdata->boosti);
	if (ret) {
		pdata->boosti = 1200;
		pr_err("Failed to read node of ti,sy6970,boost-current\n");
	}

	return pdata;
}

static int sy6970_get_charger_type(struct sy6970 *bq, enum power_supply_type *type)
{
	int ret = 0;
	u8 reg_val = 0;
	int vbus_stat = 0;
	enum power_supply_type oplus_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;

	ret = sy6970_read_byte(bq, SY6970_REG_0B, &reg_val);
	if (ret)
		return ret;

	vbus_stat = (reg_val & SY6970_VBUS_STAT_MASK);
	vbus_stat >>= SY6970_VBUS_STAT_SHIFT;
	bq->vbus_type = vbus_stat;
	chg_info("type:%d,reg0B = 0x%x\n",vbus_stat,reg_val);

	switch (vbus_stat) {
	case SY6970_VBUS_TYPE_NONE:
		oplus_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
		break;
	case SY6970_VBUS_TYPE_SDP:
		if (oplus_pd_without_usb()) {
			chg_err("pd without usb_comm,force sdp to dcp\n");
			oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
			g_bq->is_force_aicl = false;
		} else {
			oplus_chg_type = POWER_SUPPLY_TYPE_USB;
		}
		break;
	case SY6970_VBUS_TYPE_CDP:
		if (oplus_pd_without_usb()) {
			chg_err("pd without usb_comm,force cdp to dcp\n");
			oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
			g_bq->is_force_aicl = false;
		} else {
			oplus_chg_type = POWER_SUPPLY_TYPE_USB_CDP;
		}
		break;
	case SY6970_VBUS_TYPE_DCP:
		oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	case SY6970_VBUS_TYPE_HVDCP:
		oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		if(!disable_QC){
			bq->hvdcp_can_enabled = true;
		}
		break;
	case SY6970_VBUS_TYPE_UNKNOWN:
		oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	case SY6970_VBUS_TYPE_NON_STD:
		oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	default:
		oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
		break;
	}

	*type = oplus_chg_type;
	oplus_set_usb_props_type(oplus_chg_type);
	return 0;
}

static int sy6970_inform_charger_type(struct sy6970 *bq)
{
	int ret = 0;

#ifdef CONFIG_OPLUS_CHARGER_MTK
	union power_supply_propval propval;

	if (!bq->psy) {
		bq->psy = power_supply_get_by_name("mtk-master-charger");
		if (IS_ERR_OR_NULL(bq->psy)) {
			chg_err("Couldn't get bq->psy");
			return -EINVAL;
		}
	}

	if (bq->power_good)
		propval.intval = 1;
	else
		propval.intval = 0;

	chg_info("inform power supply online status:%d", propval.intval);
	ret = power_supply_set_property(bq->psy, POWER_SUPPLY_PROP_ONLINE, &propval);
	if (ret < 0) {
		chg_err("inform power supply online failed:%d", ret);
	}

	power_supply_changed(bq->psy);
#else
	oplus_chg_wake_update_work();
#endif
	return ret;
}


static int sy6970_cfg_dpdm2hiz_mode(struct sy6970 *bq)
{
	int ret = 0;
	u8 reg_val = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_01, &reg_val);
	chg_debug("(%d,%d)", reg_val, ret);

	return ret;
}

static int sy6970_request_dpdm(struct sy6970 *chip, bool enable)
{
        int ret = 0;

        if (!chip) {
		chg_err(" chip is null");
                return 0;
	}

        /* fetch the DPDM regulator */
        if (!chip->dpdm_reg && of_get_property(chip->dev->of_node,
                                "dpdm-supply", NULL)) {
                chip->dpdm_reg = devm_regulator_get(chip->dev, "dpdm");
                if (IS_ERR(chip->dpdm_reg)) {
                        ret = PTR_ERR(chip->dpdm_reg);
                        chg_err("Couldn't get dpdm regulator ret=%d\n", ret);
                        chip->dpdm_reg = NULL;
                        return ret;
                }
        }

        mutex_lock(&chip->dpdm_lock);
        if (enable) {
                if (chip->dpdm_reg && !chip->dpdm_enabled) {
                        chg_err("enabling DPDM regulator\n");
                        ret = regulator_enable(chip->dpdm_reg);
                        if (ret < 0) {
                                chg_err("Couldn't enable dpdm regulator ret=%d\n", ret);
			} else {
                                chip->dpdm_enabled = true;
			}
                }
        } else {
                if (chip->dpdm_reg && chip->dpdm_enabled) {
                        chg_err("disabling DPDM regulator\n");
                        ret = regulator_disable(chip->dpdm_reg);
                        if (ret < 0) {
                                chg_err("Couldn't disable dpdm regulator ret=%d\n", ret);
			} else {
                                chip->dpdm_enabled = false;
			}
                }
        }
        mutex_unlock(&chip->dpdm_lock);

	chg_info("dpdm regulator: enable= %d, ret=%d\n", enable, ret);
        return ret;
}

#define DEFAULT_IBUS_MA		500
static irqreturn_t sy6970_irq_handler(int irq, void *data)
{
	struct sy6970 *bq = (struct sy6970 *)data;
	int ret = 0;
	u8 reg_val = 0;
	u8 hz_mode = 0;
	bool prev_pg = false;
	enum power_supply_type prev_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
	enum power_supply_type cur_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
	struct oplus_chg_chip *chip = g_oplus_chip;

	if (chip == NULL) {
		chg_err("chip is null");
		return IRQ_HANDLED;
	}

	ret = sy6970_read_byte(bq, SY6970_REG_0B, &reg_val);
	if (ret) {
		chg_err("read register failed.");
		return IRQ_HANDLED;
	}

	prev_pg = bq->power_good;
	bq->power_good = !!(reg_val & SY6970_PG_STAT_MASK);
	chg_info("(%d,%d)\n",prev_pg,bq->power_good);
	oplus_sy6970_set_mivr_by_battery_vol();

	if (dumpreg_by_irq)
		sy6970_dump_regs(bq);

	if (!prev_pg && bq->power_good) {
		if (!bq->chg_det_enable)
			return IRQ_HANDLED;

		chg_info("adapter/usb inserted.");

		/*step1: eable ilim and set input current to protect the charger.*/
		/*
                * Enable the ilim to protect the charger,
                * because of the charger input current limit will change to default value when bc1.2 complete..
                */
		sy6970_enable_enlim(bq);
		bq->is_force_aicl = true;
		sy6970_set_input_current_limit(bq, DEFAULT_IBUS_MA);

		/*step2: start 5s thread */
		oplus_chg_wake_update_work();

		/*step3: BC1.2*/
		chg_debug("adapter/usb inserted. start bc1.2");
		Charger_Detect_Init();
		if (bq->is_force_dpdm) {
			bq->is_force_dpdm = false;
			sy6970_force_dpdm(bq, false);
		} else {
			sy6970_disable_hvdcp(bq);
			sy6970_force_dpdm(bq, true);
		}
		sy6970_enable_auto_dpdm(bq,false);

		bq->chg_need_check = true;
		bq->chg_start_check = false;
		bq->hvdcp_checked = false;

		/*Step4: check SDP/CDP, retry BC1.2*/
		chg_debug("wake up the chgtype thread.");
		wake_up_interruptible(&oplus_chgtype_wq);

		oplus_wake_up_usbtemp_thread();
	} else if (prev_pg && !bq->power_good) {
		bq->chg_cur = 0;
		bq->aicr = DEFAULT_IBUS_MA;
		bq->qc_to_9v_count = 0;

		oplus_wake_up_usbtemp_thread();
		ret = sy6970_get_hiz_mode(bq,&hz_mode);
		if (!ret && hz_mode) {
			chg_err("hiz mode ignore\n");
			return IRQ_HANDLED;
		}
		bq->is_force_aicl = false;
		bq->pre_current_ma = -1;
		bq->usb_connect_start = false;
		bq->hvdcp_can_enabled = false;
		bq->hvdcp_checked = false;
		bq->sdp_retry = false;
		bq->cdp_retry = false;
		bq->is_force_dpdm = false;
		bq->retry_hvdcp_algo = false;
		bq->chg_type = CHARGER_UNKNOWN;
		bq->oplus_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
		oplus_set_usb_props_type(bq->oplus_chg_type);
		bq->nonstand_retry_bc = false;
		bq->is_bc12_end = true;
		if (chip) {
			chip->pd_chging = false;
		}
		sy6970_inform_charger_type(bq);
		if (chip->is_double_charger_support) {
                        chip->sub_chg_ops->charging_disable();
                }

		sy6970_enable_enlim(bq);
		sy6970_disable_hvdcp(bq);
		sy6970_cfg_dpdm2hiz_mode(bq);

		Charger_Detect_Release();
		cancel_delayed_work_sync(&bq->sy6970_retry_adapter_detection);
		cancel_delayed_work_sync(&bq->sy6970_aicr_setting_work);
		cancel_delayed_work_sync(&bq->sy6970_hvdcp_bc12_work);
		oplus_chg_wake_update_work();
		chg_info("adapter/usb removed.");
		return IRQ_HANDLED;
	} else if (!prev_pg && !bq->power_good) {
		chg_info("prev_pg & now_pg is false\n");
		return IRQ_HANDLED;
	}

	if (bq->otg_enable){
		chg_info("otg_enable\n");

		sy6970_disable_enlim(bq);
		return IRQ_HANDLED;
	}

	if (!(SY6970_VBUS_STAT_MASK & reg_val)) {
		return IRQ_HANDLED;
	}

	/*step5: get charger type.*/
	prev_chg_type = bq->oplus_chg_type;
	ret = sy6970_get_charger_type(bq, &cur_chg_type);
	chg_info(" prev_chg_type %d --> cur_chg_type %d, bq->oplus_chg_type %d, chg_det_enable %d, ret %d\n",
		prev_chg_type, cur_chg_type, bq->oplus_chg_type, bq->chg_det_enable,ret);

	/* Fix the bug : the adapter type is changed  from DCP to SDP/CDP when doing hvdcp BC1.2 */
	if ((prev_chg_type == POWER_SUPPLY_TYPE_USB_DCP)
		&& ((cur_chg_type == POWER_SUPPLY_TYPE_USB) || (cur_chg_type == POWER_SUPPLY_TYPE_USB_CDP))) {
		chg_info(" keep cur_chg_type %d as prev_chg_type %d ", cur_chg_type, prev_chg_type);
		cur_chg_type = prev_chg_type;
	}

	if ((cur_chg_type == POWER_SUPPLY_TYPE_USB)
		|| (cur_chg_type == POWER_SUPPLY_TYPE_USB_CDP)) {
		chg_info(" type usb %d\n", bq->usb_connect_start);
		bq->oplus_chg_type = cur_chg_type;

		/*QCM Platform Need to notify the TYPEC*/
		oplus_start_usb_peripheral();

		/*Step 6.1 CDP/SDP */
		if (bq->usb_connect_start) {
			/*Charger_Detect_Release();*/
			sy6970_disable_enlim(bq);
			bq->is_force_aicl = false;
			sy6970_inform_charger_type(bq);
		}

		oplus_set_usb_props_type(bq->oplus_chg_type);
		oplus_chg_wake_update_work();
	} else if (cur_chg_type != POWER_SUPPLY_TYPE_UNKNOWN) {
		/*Step 6.2: DCP/HVDCP*/
		chg_info(" cur_chg_type = %d, vbus_type = %d\n", cur_chg_type, bq->vbus_type);
		/*Charger_Detect_Release();*/

		bq->oplus_chg_type = cur_chg_type;

		bq->is_force_aicl = false;
		sy6970_inform_charger_type(bq);

		/*Step7: HVDCP and BC1.2*/
		if (!bq->hvdcp_checked && !sy6970_is_hvdcp(bq)) {
			chg_info(" enable hvdcp.");
			if (!sy6970_is_dcp(bq)) {
				chg_debug(" not dcp.");
			}

			schedule_delayed_work(&bq->sy6970_hvdcp_bc12_work, msecs_to_jiffies(1500));
		} else if (bq->hvdcp_checked) {
			chg_info(" sy6970 hvdcp is checked");

			/*Step8: HVDCP AICL and config.*/
			if (bq->hvdcp_can_enabled) {
				chg_info(" sy6970 hvdcp_can_enabled.");
			}

			/*restart AICL after the BC1.2 of HDVCP check*/
			schedule_delayed_work(&bq->sy6970_aicr_setting_work, 0);
			oplus_set_usb_props_type(bq->oplus_chg_type);
			oplus_chg_wake_update_work();
		} else {
			chg_err("oplus_chg_type = %d, hvdcp_checked = %d", bq->oplus_chg_type, bq->hvdcp_checked);
		}
	} else {
		chg_err("oplus_chg_type = %d, vbus_type = %d", bq->oplus_chg_type, bq->vbus_type);
	}

	return IRQ_HANDLED;
}

static oplus_chg_irq_gpio_init(struct sy6970 *sy)
{
	int rc = 0;
	struct device_node *node = sy->dev->of_node;

	if (!node) {
		chg_err("device tree node missing\n");
		return -EINVAL;
	}

	/*irq_gpio*/
	sy->irq_gpio = of_get_named_gpio(node, "qcom,chg_irq_gpio", 0);
	if (!gpio_is_valid(sy->irq_gpio)) {
		chg_err("sy->irq_gpio not specified\n");
		return -EINVAL;
	}

	rc = gpio_request(sy->irq_gpio, "chg_irq_gpio");
	if (rc) {
		chg_err("unable to request gpio [%d]\n", sy->irq_gpio);
	}
	chg_err("sy->irq_gpio =%d\n", sy->irq_gpio);

	/*irq_num */
	sy->irq = gpio_to_irq(sy->irq_gpio);
	chg_debug(" gpio_to_irq(sy->irq) =%d\n", sy->irq);

	/* set splitchg pinctrl*/
	sy->pinctrl = devm_pinctrl_get(sy->dev);
	if (IS_ERR_OR_NULL(sy->pinctrl)) {
		chg_err("get pinctrl fail\n");
		return -EINVAL;
	}

	sy->splitchg_inter_active = pinctrl_lookup_state(sy->pinctrl, "splitchg_inter_active");
	if (IS_ERR_OR_NULL(sy->splitchg_inter_active)) {
		chg_err(": Failed to get the active state pinctrl handle\n");
		return -EINVAL;
	}

	sy->splitchg_inter_sleep = pinctrl_lookup_state(sy->pinctrl, "splitchg_inter_sleep");
	if (IS_ERR_OR_NULL(sy->splitchg_inter_sleep)) {
		chg_err(" Failed to get the sleep state pinctrl handle\n");
		return -EINVAL;
	}

	//irq active
	gpio_direction_input(sy->irq_gpio);
	pinctrl_select_state(sy->pinctrl, sy->splitchg_inter_active); /* no_PULL */
	rc = gpio_get_value(sy->irq_gpio);

	chg_info("irq sy->irq_gpio input = %d irq_gpio_stat = %d\n", sy->irq_gpio, rc);
	return 0;
}


static int sy6970_register_interrupt(struct device_node *np,struct sy6970 *bq)
{
	int ret = 0;

#ifdef CONFIG_OPLUS_CHARGER_MTK
	bq->irq = irq_of_parse_and_map(np, 0);
#else
	ret = oplus_chg_irq_gpio_init(bq);
	if (ret) {
		return ret;
	}
#endif
	ret = devm_request_threaded_irq(bq->dev, bq->irq, NULL,
					sy6970_irq_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					bq->eint_name, bq);
	if (ret < 0) {
		pr_err("request thread irq failed:%d\n", ret);
		return ret;
	}

	enable_irq_wake(bq->irq);

	return 0;
}

/*static void sy6970_enter_hz_work_handler(struct work_struct *work) {
        chg_debug("enter hz mode!");
	oplus_sy6970_charger_suspend();

#ifndef CONFIG_OPLUS_CHARGER_MTK
	sy6970_request_dpdm(g_bq, true);
	oplus_start_usb_peripheral();
#endif
}*/

static int sy6970_init_device(struct sy6970 *bq)
{
	int ret = 0;

	sy6970_disable_watchdog_timer(bq);
	bq->is_force_dpdm = false;
	ret = sy6970_set_prechg_current(bq, bq->platform_data->iprechg);
	if (ret)
		pr_err("Failed to set prechg current, ret = %d\n", ret);

	ret = sy6970_set_chargevolt(bq, DEFAULT_CV);
	if (ret)
		pr_err("Failed to set default cv, ret = %d\n", ret);

	ret = sy6970_set_term_current(bq, bq->platform_data->iterm);
	if (ret)
		pr_err("Failed to set termination current, ret = %d\n", ret);

	ret = sy6970_set_boost_voltage(bq, bq->platform_data->boostv);
	if (ret)
		pr_err("Failed to set boost voltage, ret = %d\n", ret);

	ret = sy6970_set_boost_current(bq, bq->platform_data->boosti);
	if (ret)
		pr_err("Failed to set boost current, ret = %d\n", ret);

	ret = sy6970_enable_enlim(g_bq);
	if (ret)
		pr_err("Failed to set enlim, ret = %d\n", ret);

	ret = sy6970_enable_auto_dpdm(bq,false);
	if (ret)
		pr_err("Failed to set auto dpdm, ret = %d\n", ret);

	ret = sy6970_vmin_limit(bq);
	if (ret)
		pr_err("Failed to set vmin limit, ret = %d\n", ret);

	ret = sy6970_adc_start_one_shot(bq);
	if (ret)
		pr_err("Failed to start adc, ret = %d\n", ret);


	ret = sy6970_set_input_volt_limit(bq, HW_AICL_POINT_VOL_5V_PHASE1);
	if (ret)
		pr_err("Failed to set input volt limit, ret = %d\n", ret);

	return 0;
}

static bool sy6970_is_dcp(struct sy6970 *bq)
{
        int ret = 0;
        u8 reg_val = 0;
        int vbus_stat = 0;

        ret = sy6970_read_byte(bq, SY6970_REG_0B, &reg_val);
        if (ret) {
                return false;
        }

        vbus_stat = (reg_val & SY6970_VBUS_STAT_MASK);
        vbus_stat >>= SY6970_VBUS_STAT_SHIFT;
        if (vbus_stat == SY6970_VBUS_TYPE_DCP)
                return true;

        return false;
}


static bool sy6970_is_hvdcp(struct sy6970 *bq)
{
	int ret = 0;
	u8 reg_val = 0;
	int vbus_stat = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_0B, &reg_val);
	if (ret) {
		return false;
	}

	vbus_stat = (reg_val & SY6970_VBUS_STAT_MASK);
	vbus_stat >>= SY6970_VBUS_STAT_SHIFT;
	if (vbus_stat == SY6970_VBUS_TYPE_HVDCP)
		return true;

	return false;
}

static bool sy6970_is_usb(struct sy6970 *bq)
{
	int ret = 0;
	u8 reg_val = 0;
	int vbus_stat = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_0B, &reg_val);
	if (ret) {
		return false;
	}

	vbus_stat = (reg_val & SY6970_VBUS_STAT_MASK);
	vbus_stat >>= SY6970_VBUS_STAT_SHIFT;

	if (vbus_stat == SY6970_VBUS_TYPE_SDP) {
		return true;
	}

	return false;
}

static void determine_initial_status(struct sy6970 *bq)
{
	enum power_supply_type type = POWER_SUPPLY_TYPE_UNKNOWN;
	if (sy6970_is_hvdcp(bq)){
		sy6970_get_charger_type(bq, &type);
		sy6970_inform_charger_type(bq);
	}
}

static int sy6970_detect_device(struct sy6970 *bq)
{
	int ret = 0;
	u8 data = 0;

	ret = sy6970_read_byte(bq, SY6970_REG_14, &data);
	if (!ret) {
		bq->part_no = (data & SY6970_PN_MASK) >> SY6970_PN_SHIFT;
		bq->revision =
		    (data & SY6970_DEV_REV_MASK) >> SY6970_DEV_REV_SHIFT;
	}

	pr_notice("bq->part_no = 0x%08x revision=%d\n", bq->part_no, bq->revision);

	if (bq->part_no == 0x01) {
		bq->is_sy6970 = true;
		pr_notice("----SY6970 IC----\n");
	} else {
		bq->is_sy6970 = false;
		pr_notice("----Not SY6970 IC-----\n");
	}

	return ret;
}

static void sy6970_dump_regs(struct sy6970 *bq)
{
	int addr = 0;
	u8 val[25];
	int ret = 0;
	char buf[400];
	char *s = buf;

	for (addr = SY6970_REG_00; addr <= SY6970_REG_14; addr++) {
		ret = sy6970_read_byte(bq, addr, &val[addr]);
		msleep(1);
	}

	s+=sprintf(s,"sy6970_dump_regs:");
	for (addr = SY6970_REG_00; addr <= SY6970_REG_14; addr++){
		s+=sprintf(s,"[0x%.2x,0x%.2x]", addr, val[addr]);
	}
	s+=sprintf(s,"\n");

	chg_info("%s \n", buf);

	return;
}

static ssize_t
sy6970_show_registers(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct sy6970 *bq = dev_get_drvdata(dev);
	u8 addr;
	u8 val;
	u8 tmpbuf[200];
	int len;
	int idx = 0;
	int ret;

	idx = snprintf(buf, PAGE_SIZE, "%s:\n", "sy6970 Reg");
	for (addr = SY6970_REG_00; addr <= SY6970_REG_14; addr++) {
		ret = sy6970_read_byte(bq, addr, &val);
		if (ret == 0) {
			len = snprintf(tmpbuf, PAGE_SIZE - idx,
				       "Reg[%.2x] = 0x%.2x\n", addr, val);
			memcpy(&buf[idx], tmpbuf, len);
			idx += len;
		}
	}

	return idx;
}

static ssize_t
sy6970_store_registers(struct device *dev,
			struct device_attribute *attr, const char *buf,
			size_t count)
{
	struct sy6970 *bq = dev_get_drvdata(dev);
	int ret;
	unsigned int reg;
	unsigned int val;

	ret = sscanf(buf, "%x %x", &reg, &val);
	if (ret == 2 && reg < 0x14) {
		sy6970_write_byte(bq, (unsigned char) reg,
				   (unsigned char) val);
	}

	return count;
}

static DEVICE_ATTR(registers, S_IRUGO | S_IWUSR, sy6970_show_registers,
		   sy6970_store_registers);

static struct attribute *sy6970_attributes[] = {
	&dev_attr_registers.attr,
	NULL,
};

static const struct attribute_group sy6970_attr_group = {
	.attrs = sy6970_attributes,
};

#ifdef CONFIG_OPLUS_CHARGER_MTK
static int sy6970_charging(struct charger_device *chg_dev, bool enable)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	int ret = 0;
	u8 val = 0;

	if (enable)
		ret = sy6970_enable_charger(bq);
	else
		ret = sy6970_disable_charger(bq);

	chg_debug(" %s charger %s\n", enable ? "enable" : "disable",
	       !ret ? "successfully" : "failed");

	ret = sy6970_read_byte(bq, SY6970_REG_03, &val);
	if (!ret)
		bq->charge_enabled = !!(val & SY6970_CHG_CONFIG_MASK);

	return ret;
}

static int sy6970_plug_in(struct charger_device *chg_dev)
{
	int ret = 0;

	ret = sy6970_charging(chg_dev, true);
	if (ret) {
		chg_err("Failed to enable charging:%d", ret);
	}

	return ret;
}

static int sy6970_plug_out(struct charger_device *chg_dev)
{
	int ret = 0;

	ret = sy6970_charging(chg_dev, false);

	if (ret) {
		chg_err("Failed to disable charging:%d", ret);
	}
	return ret;
}

static int sy6970_dump_register(struct charger_device *chg_dev)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

	sy6970_dump_regs(bq);

	return 0;
}

static int sy6970_is_charging_enable(struct charger_device *chg_dev, bool *en)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

	*en = bq->charge_enabled;

	return 0;
}

static int sy6970_is_charging_done(struct charger_device *chg_dev, bool *done)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	int ret;

	ret = sy6970_check_charge_done(bq, done);

	return ret;
}

static int sy6970_set_ichg(struct charger_device *chg_dev, u32 curr)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	chg_debug("charge curr = %d", curr);
	return sy6970_set_chargecurrent(bq, curr / UNIT_TRANS_1000);
}
#endif

static int _sy6970_get_ichg(struct sy6970 *bq, u32 *curr)
{
	u8 reg_val = 0;
	int ichg = 0;
	int ret = 0;

	sy6970_adc_start_one_shot(bq);
	ret = sy6970_read_byte(bq, SY6970_REG_04, &reg_val);
	if (!ret) {
		ichg = (reg_val & SY6970_ICHG_MASK) >> SY6970_ICHG_SHIFT;
		ichg = ichg * SY6970_ICHG_LSB + SY6970_ICHG_BASE;
		*curr = ichg * UNIT_TRANS_1000;
	}

	return ret;
}

static u32 oplus_get_charger_current(void) {
        u32 value = 0;

        if (!g_bq) {
                return 0;
        }

        _sy6970_get_ichg(g_bq, &value);
        return value;
}

#ifdef CONFIG_OPLUS_CHARGER_MTK
static int sy6970_get_min_ichg(struct charger_device *chg_dev, u32 *curr)
{
	*curr = 60 * UNIT_TRANS_1000;

	return 0;
}

static int sy6970_get_ichg(struct charger_device *chg_dev, u32 *curr)
{
        struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
        u8 reg_val;
        int ichg;
        int ret;

        ret = _sy6970_get_ichg(bq, curr);
        return ret;
}

static int sy6970_set_vchg(struct charger_device *chg_dev, u32 volt)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

	chg_debug("charge volt = %d", volt);

	return sy6970_set_chargevolt(bq, volt / UNIT_TRANS_1000);
}

static int sy6970_get_vchg(struct charger_device *chg_dev, u32 *volt)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	u8 reg_val;
	int vchg;
	int ret;

	ret = sy6970_read_byte(bq, SY6970_REG_06, &reg_val);
	if (!ret) {
		vchg = (reg_val & SY6970_VREG_MASK) >> SY6970_VREG_SHIFT;
		vchg = vchg * SY6970_VREG_LSB + SY6970_VREG_BASE;
		*volt = vchg * UNIT_TRANS_1000;
	}

	return ret;
}

static int sy6970_set_ivl(struct charger_device *chg_dev, u32 volt)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

	chg_debug("vindpm volt = %d", volt);

	return sy6970_set_input_volt_limit(bq, volt / UNIT_TRANS_1000);

}

static int sy6970_set_icl(struct charger_device *chg_dev, u32 curr)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

	chg_debug("indpm curr = %d", curr);

	return sy6970_set_input_current_limit(bq, curr / UNIT_TRANS_1000);
}

static int sy6970_get_icl(struct charger_device *chg_dev, u32 *curr)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	u8 reg_val;
	int icl;
	int ret;

	ret = sy6970_read_byte(bq, SY6970_REG_00, &reg_val);
	if (!ret) {
		icl = (reg_val & SY6970_IINLIM_MASK) >> SY6970_IINLIM_SHIFT;
		icl = icl * SY6970_IINLIM_LSB + SY6970_IINLIM_BASE;
		*curr = icl * UNIT_TRANS_1000;
	}

	return ret;

}

static int sy6970_kick_wdt(struct charger_device *chg_dev)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

	return sy6970_reset_watchdog_timer(bq);
}

static int sy6970_set_otg(struct charger_device *chg_dev, bool en)
{
	int ret;
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

	if (en) {
		sy6970_disable_charger(bq);
		ret = sy6970_enable_otg(bq);
	} else {
		ret = sy6970_disable_otg(bq);
		sy6970_enable_charger(bq);
	}
	if (!ret)
		bq->otg_enable = en;

	chg_info(" %s OTG %s\n", en ? "enable" : "disable",
	       !ret ? "successfully" : "failed");

	return ret;
}

static int sy6970_set_safety_timer(struct charger_device *chg_dev, bool en)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	int ret;

	if (en)
		ret = sy6970_enable_safety_timer(bq);
	else
		ret = sy6970_disable_safety_timer(bq);

	return ret;
}

static int sy6970_is_safety_timer_enabled(struct charger_device *chg_dev,
					   bool *en)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	int ret;
	u8 reg_val;

	ret = sy6970_read_byte(bq, SY6970_REG_07, &reg_val);

	if (!ret)
		*en = !!(reg_val & SY6970_EN_TIMER_MASK);

	return ret;
}

static int sy6970_set_boost_ilmt(struct charger_device *chg_dev, u32 curr)
{
	struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);
	int ret;

	chg_debug("otg curr = %d", curr);

	ret = sy6970_set_boost_current(bq, curr / UNIT_TRANS_1000);

	return ret;
}

static int sy6970_enable_chgdet(struct charger_device *chg_dev, bool en)
{
        int ret;
        struct sy6970 *bq = dev_get_drvdata(&chg_dev->dev);

        chg_debug("sy6970_enable_chgdet:%d",en);

        ret = sy6970_chgdet_en(bq, en);

        return ret;
}

#endif


static int sy6970_chgdet_en(struct sy6970 *bq, bool en)
{
	int ret = 0;
	struct oplus_chg_chip *chip = g_oplus_chip;

	if ((chip == NULL) || (NULL == bq) ) {
		return -1;
	}

	chg_debug(" enable:%d\n",en);
	bq->chg_det_enable = en;

	if (en) {
		Charger_Detect_Init();
		oplus_for_cdp();
                sy6970_enable_auto_dpdm(bq,false);
                // bq->is_force_aicl = true;
                bq->is_retry_bc12 = true;
                sy6970_force_dpdm(bq, true);
		bq->is_force_dpdm = true;
	} else {
		bq->pre_current_ma = -1;
		bq->hvdcp_can_enabled = false;
		bq->hvdcp_checked = false;
		bq->retry_hvdcp_algo = false;
		bq->nonstand_retry_bc = false;
		bq->sdp_retry = false;
		bq->cdp_retry = false;
		bq->usb_connect_start = false;
		bq->is_force_aicl = false;
                bq->is_force_dpdm = false;

		sy6970_disable_hvdcp(bq);
		Charger_Detect_Release();

		memset(&bq->ptime[0], 0, sizeof(struct timespec));
		memset(&bq->ptime[1], 0, sizeof(struct timespec));
		cancel_delayed_work_sync(&bq->sy6970_retry_adapter_detection);
		cancel_delayed_work_sync(&bq->sy6970_aicr_setting_work);
                cancel_delayed_work_sync(&bq->sy6970_hvdcp_bc12_work);

		bq->chg_type = CHARGER_UNKNOWN;
		bq->oplus_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
		oplus_set_usb_props_type(bq->oplus_chg_type);
		if (chip->is_double_charger_support && chip->sub_chg_ops) {
			chip->sub_chg_ops->charging_disable();
		}

		sy6970_inform_charger_type(bq);
	}

	return ret;
}

static int sy6970_enter_ship_mode(struct sy6970 *bq, bool en)
{
	int ret;
	u8 val;

	if (en)
		val = SY6970_BATFET_OFF;
	else
		val = SY6970_BATFET_ON;
	val <<= SY6970_BATFET_DIS_SHIFT;

	ret = sy6970_update_bits(bq, SY6970_REG_09,
						SY6970_BATFET_DIS_MASK, val);
	return ret;

}

static int sy6970_enable_shipmode(bool en)
{
	int ret;

	ret = sy6970_enter_ship_mode(g_bq, en);

	return 0;
}

static int sy6970_set_hz_mode(bool en)
{
	int ret;

	if (en) {
		ret = sy6970_enter_hiz_mode(g_bq);
	} else {
		ret = sy6970_exit_hiz_mode(g_bq);
	}

	return ret;
}

static bool oplus_usbtemp_condition(void) {
	if ((g_bq && g_bq->power_good)
		|| ( g_oplus_chip && g_oplus_chip->charger_exist)) {
		return true;
	} else {
		return false;
	}
}

#ifdef CONFIG_OPLUS_CHARGER_MTK
static struct charger_ops sy6970_chg_ops = {
	/* Normal charging */
	.plug_in = sy6970_plug_in,
	.plug_out = sy6970_plug_out,
	.dump_registers = sy6970_dump_register,
	.enable = sy6970_charging,
	.is_enabled = sy6970_is_charging_enable,

	.get_charging_current = sy6970_get_ichg,
	.set_charging_current = sy6970_set_ichg,
	.get_input_current = sy6970_get_icl,
	.set_input_current = sy6970_set_icl,
	.get_constant_voltage = sy6970_get_vchg,
	.set_constant_voltage = sy6970_set_vchg,
	.kick_wdt = sy6970_kick_wdt,
	.set_mivr = sy6970_set_ivl,
	.is_charging_done = sy6970_is_charging_done,
	.get_min_charging_current = sy6970_get_min_ichg,

	/* Safety timer */
	.enable_safety_timer = sy6970_set_safety_timer,
	.is_safety_timer_enabled = sy6970_is_safety_timer_enabled,

	/* Power path */
	.enable_powerpath = NULL,
	.is_powerpath_enabled = NULL,

	.enable_chg_type_det = sy6970_enable_chgdet,
	/* OTG */
	.enable_otg = sy6970_set_otg,
	.set_boost_current_limit = sy6970_set_boost_ilmt,
	.enable_discharge = NULL,
	
	/* PE+/PE+20 */
	.send_ta_current_pattern = NULL,
	.set_pe20_efficiency_table = NULL,
	.send_ta20_current_pattern = NULL,
	.reset_ta = NULL,
	.enable_cable_drop_comp = NULL,

	/* ADC */
	.get_tchg_adc = get_chgntc_adc_temp,
	.get_vbus_adc = sy6970_get_vbus_adc,
};
#endif

static int oplus_sy6970_set_icl(struct sy6970 *bq, u32 curr)
{
	chg_debug("indpm curr = %d", curr);

	return sy6970_set_input_current_limit(bq, curr / UNIT_TRANS_1000);
}

static int oplus_sy6970_get_icl(struct sy6970 *bq, u32 *curr)
{
	u8 reg_val;
	int icl;
	int ret;

	ret = sy6970_read_byte(bq, SY6970_REG_00, &reg_val);
	if (!ret) {
		icl = (reg_val & SY6970_IINLIM_MASK) >> SY6970_IINLIM_SHIFT;
		icl = icl * SY6970_IINLIM_LSB + SY6970_IINLIM_BASE;
		*curr = icl * UNIT_TRANS_1000;
	}

	return ret;
}

void oplus_sy6970_dump_registers(void)
{
	sy6970_dump_regs(g_bq);

	if ((g_oplus_chip != NULL) && (g_oplus_chip->is_double_charger_support)) {
		if (g_oplus_chip->sub_chg_ops) {
			g_oplus_chip->sub_chg_ops->kick_wdt();
			g_oplus_chip->sub_chg_ops->dump_registers();
		}
	}
}

int oplus_sy6970_kick_wdt(void)
{
	return sy6970_reset_watchdog_timer(g_bq);
}

void oplus_sy6970_set_mivr(int vbatt)
{
	int charger_vol = 0;
	charger_vol = oplus_sy6970_get_vbus();
	if (g_bq->hw_aicl_point == HW_AICL_POINT_VOL_5V_PHASE1 && vbatt > AICL_POINT_VOL_5V) {
		g_bq->hw_aicl_point = HW_AICL_POINT_VOL_5V_PHASE2;
	} else if (g_bq->hw_aicl_point == HW_AICL_POINT_VOL_5V_PHASE2 && vbatt < AICL_POINT_VOL_5V) {
		g_bq->hw_aicl_point = HW_AICL_POINT_VOL_5V_PHASE1;
	}

	sy6970_set_input_volt_limit(g_bq, g_bq->hw_aicl_point);

	if (g_oplus_chip->is_double_charger_support) {
		g_oplus_chip->sub_chg_ops->set_aicl_point(vbatt);
	}
}

#define SY6970_VINDPM_VBAT_PHASE1	4300
#define SY6970_VINDPM_VBAT_PHASE2	4200
#define SY6970_VINDPM_THRES_PHASE1	400
#define SY6970_VINDPM_THRES_PHASE2	300
#define SY6970_VINDPM_THRES_PHASE3	200
#define SY6970_VINDPM_THRES_MIN		4400
void oplus_sy6970_set_mivr_by_battery_vol(void)
{
	u32 mV =0;
	int vbatt = 0;
	struct oplus_chg_chip *chip = g_oplus_chip;

	if (chip) {
		vbatt = chip->batt_volt;
	}

	if (vbatt > SY6970_VINDPM_VBAT_PHASE1){
		mV = vbatt + SY6970_VINDPM_THRES_PHASE1;
	} else if (vbatt > SY6970_VINDPM_VBAT_PHASE2){
		mV = vbatt + SY6970_VINDPM_THRES_PHASE2;
	} else {
		mV = vbatt + SY6970_VINDPM_THRES_PHASE3;
	}

	if (mV < SY6970_VINDPM_THRES_MIN) {
		mV = SY6970_VINDPM_THRES_MIN;
	}
	sy6970_set_input_volt_limit(g_bq, mV);

	return;
}

static int usb_icl[] = {
	100, 500, 900, 1200, 1500, 1750, 2000, 3000,
};

#define FULL_PCT	100
#define AICL_DELAY_MS	90
#define AICL_DELAY2_MS	120
#define EM_MODE_ICHG_MA	3400
static int oplus_sy6970_set_aicr(int current_ma)
{
	struct oplus_chg_chip *chip = g_oplus_chip;
	int rc = 0, i = 0;
	int chg_vol = 0;
	int aicl_point = 0;
	int aicl_point_temp = 0;
	int main_cur = 0;
	int slave_cur = 0;
	g_bq->pre_current_ma = current_ma;
	g_bq->aicr = current_ma;

	if (g_bq->is_force_aicl) {
		chg_info("is_force_aicl current_ma = %d mA", current_ma);
		return 0;
	}

	if (atomic_read(&g_bq->charger_suspended) == 1) {
		chg_err("suspend,ignore set current=%dmA\n", current_ma);
		return 0;
	}

	if (chip->is_double_charger_support) {
		rc = chip->sub_chg_ops->charging_disable();
		if (rc < 0) {
			chg_err("disable sub charging failed");
		}
		chg_debug("disable subchg ");
	}

	chg_debug("usb input max current limit=%d, em_mode = %d", current_ma, chip->em_mode);
	if (chip && chip->is_double_charger_support) {
		chg_vol = oplus_sy6970_get_vbus();
		if (chg_vol > AICL_POINT_VOL_9V) {
			aicl_point_temp = aicl_point = AICL_POINT_VOL_9V;
		} else {
			if (chip->batt_volt > AICL_POINT_VOL_5V)
				aicl_point_temp = aicl_point = SW_AICL_POINT_VOL_5V_PHASE2;
			else
				aicl_point_temp = aicl_point = SW_AICL_POINT_VOL_5V_PHASE1;
		}
	} else {
		if (chip->batt_volt > AICL_POINT_VOL_5V)
			aicl_point_temp = aicl_point = SW_AICL_POINT_VOL_5V_PHASE2;
		else
			aicl_point_temp = aicl_point = SW_AICL_POINT_VOL_5V_PHASE1;
	}

	chg_info("usb input max current limit=%d, aicl_point_temp=%d", current_ma, aicl_point_temp);
	if (current_ma < 500) {
		i = 0;
		goto aicl_end;
	}

	i = 1; /* 500 */
	sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	msleep(AICL_DELAY_MS);
	sy6970_disable_enlim(g_bq);

	chg_vol = sy6970_adc_read_vbus_volt(g_bq);
	if (chg_vol < aicl_point_temp) {
		pr_debug( "use 500 here\n");
		goto aicl_end;
	} else if (current_ma < 900)
		goto aicl_end;

	i = 2; /* 900 */
	sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	msleep(AICL_DELAY_MS);
	chg_vol = sy6970_adc_read_vbus_volt(g_bq);
	if (chg_vol < aicl_point_temp) {
		i = i - 1;
		goto aicl_pre_step;
	} else if (current_ma < 1200)
		goto aicl_end;

	i = 3; /* 1200 */
	sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	msleep(AICL_DELAY_MS);
	chg_vol = sy6970_adc_read_vbus_volt(g_bq);
	if (chg_vol < aicl_point_temp) {
		i = i - 1;
		goto aicl_pre_step;
	}

	i = 4; /* 1500 */
	aicl_point_temp = aicl_point + 50;
	sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	msleep(AICL_DELAY2_MS);
	chg_vol = sy6970_adc_read_vbus_volt(g_bq);
	if (chg_vol < aicl_point_temp) {
		i = i - 2; //We DO NOT use 1.2A here
		goto aicl_pre_step;
	} else if (current_ma < 1500) {
		i = i - 1; //We use 1.2A here
		goto aicl_end;
	} else if (current_ma < 2000)
		goto aicl_end;

	i = 5; /* 1750 */
	aicl_point_temp = aicl_point + 50;
	sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	msleep(AICL_DELAY2_MS);
	chg_vol = sy6970_adc_read_vbus_volt(g_bq);
	if (chg_vol < aicl_point_temp) {
		i = i - 2; //1.2
		goto aicl_pre_step;
	}

	i = 6; /* 2000 */
	aicl_point_temp = aicl_point;
	sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	msleep(AICL_DELAY_MS);
	if (chg_vol < aicl_point_temp) {
		i =  i - 2;//1.5
		goto aicl_pre_step;
	} else if (current_ma < 3000)
		goto aicl_end;

	i = 7; /* 3000 */
	sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	msleep(AICL_DELAY_MS);
	chg_vol = sy6970_adc_read_vbus_volt(g_bq);
	if (chg_vol < aicl_point_temp) {
		i = i - 1;
		goto aicl_pre_step;
	} else if (current_ma >= 3000)
		goto aicl_end;

aicl_pre_step:
	if ((chip->is_double_charger_support)
		&& (chip->slave_charger_enable || chip->em_mode)) {
		slave_cur = (usb_icl[i] * g_oplus_chip->slave_pct)/100;
		slave_cur -= slave_cur % 100;
		main_cur = usb_icl[i] - slave_cur;
		chg_debug("aicl: main_cur: %d,  slave_cur: %d", main_cur, slave_cur);
		sy6970_set_input_current_limit(g_bq, main_cur);

		if (chip->sub_chg_ops && chip->sub_chg_ops->input_current_write_without_aicl) {
			chip->sub_chg_ops->input_current_write_without_aicl(slave_cur);
		}

		if (chip->sub_chg_ops && chip->sub_chg_ops->charging_enable) {
			chip->sub_chg_ops->charging_enable();
		}

		if (chip->em_mode) {
                        chip->slave_charger_enable = true;
			oplus_sy6970_set_ichg(EM_MODE_ICHG_MA);
                }
	}else{
		sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	}

	chg_info("aicl_pre_step: current limit aicl chg_vol=%d j[%d]=%d sw_aicl_point:%d, main %d mA, slave %d mA, slave_charger_enable:%d\n",
		chg_vol, i, usb_icl[i], aicl_point_temp,
		main_cur, slave_cur,
		chip->slave_charger_enable);
	return rc;
aicl_end:
	if ((g_oplus_chip->is_double_charger_support)
		&& (chip->slave_charger_enable || chip->em_mode)) {
		slave_cur = (usb_icl[i] * g_oplus_chip->slave_pct)/FULL_PCT;
		slave_cur -= slave_cur % 100;
		main_cur = usb_icl[i] - slave_cur;
		chg_debug("aicl: main_cur: %d,  slave_cur: %d", main_cur, slave_cur);
		sy6970_set_input_current_limit(g_bq, main_cur);

		if (chip->sub_chg_ops && chip->sub_chg_ops->input_current_write_without_aicl) {
			chip->sub_chg_ops->input_current_write_without_aicl(slave_cur);
		}
		if (chip->sub_chg_ops && chip->sub_chg_ops->charging_enable) {
			chip->sub_chg_ops->charging_enable();
		}

		if (chip->em_mode) {
			chip->slave_charger_enable = true;
			oplus_sy6970_set_ichg(EM_MODE_ICHG_MA);
		}
	}else{
		sy6970_set_input_current_limit(g_bq, usb_icl[i]);
	}

	g_bq->is_force_aicl = false;
	chg_info("aicl_end: current limit aicl chg_vol=%d j[%d]=%d sw_aicl_point:%d, main %d mA, slave %d mA, slave_charger_enable:%d\n",
		chg_vol, i, usb_icl[i], aicl_point_temp,
		main_cur, slave_cur,
		chip->slave_charger_enable);
	return rc;
}

int oplus_sy6970_set_input_current_limit(int current_ma)
{
	if (g_bq == NULL)
		return 0;

	chg_debug(" current = %d\n", current_ma);
	oplus_sy6970_set_aicr(current_ma);
	return 0;
}

int oplus_sy6970_set_cv(int cur)
{
	return sy6970_set_chargevolt(g_bq, cur);
}

int oplus_sy6970_set_ieoc(int cur)
{
	return sy6970_set_term_current(g_bq, cur);
}

int oplus_sy6970_charging_enable(void)
{
	return sy6970_enable_charger(g_bq);
}

int oplus_sy6970_charging_disable(void)
{
	chg_info(" disable");

	sy6970_disable_watchdog_timer(g_bq);
	g_bq->pre_current_ma = -1;
	g_bq->hw_aicl_point = HW_AICL_POINT_VOL_5V_PHASE1;
	sy6970_set_input_volt_limit(g_bq, g_bq->hw_aicl_point);

	return sy6970_disable_charger(g_bq);
}

int oplus_sy6970_hardware_init(void)
{
	int ret = 0;

	chg_info(" init ");

	g_bq->hw_aicl_point = HW_AICL_POINT_VOL_5V_PHASE1;
	sy6970_set_input_volt_limit(g_bq, g_bq->hw_aicl_point);

	if (atomic_read(&g_bq->charger_suspended) == 1) {
                chg_err("suspend,ignore\n");
                return 0;
        }

	/* Enable charging */
	if (strcmp(g_bq->chg_dev_name, "primary_chg") == 0) {
		ret = sy6970_enable_charger(g_bq);
		if (ret < 0) {
			dev_notice(g_bq->dev, "%s: en chg failed\n", __func__);
		} else {
			sy6970_set_input_current_limit(g_bq, DEFAULT_IBUS_MA);
		}
	}

	if ((g_oplus_chip != NULL) && (g_oplus_chip->is_double_charger_support)) {
		if(!sy6970_is_usb(g_bq)) {
			g_oplus_chip->sub_chg_ops->hardware_init();
		} else {
			g_oplus_chip->sub_chg_ops->charging_disable();
		}
	}

	return ret;
}

int oplus_sy6970_is_charging_enabled(void)
{

	return g_bq->charge_enabled;
}

int oplus_sy6970_is_charging_done(void)
{
	bool done;

	sy6970_check_charge_done(g_bq, &done);

	return done;

}

int oplus_sy6970_enable_otg(void)
{
	int ret = 0;

	ret = sy6970_set_boost_current(g_bq, g_bq->platform_data->boosti);
	ret = sy6970_enable_otg(g_bq);

	if (ret < 0) {
		dev_notice(g_bq->dev, "%s en otg fail(%d)\n", __func__, ret);
		return ret;
	}

	g_bq->otg_enable = true;
	return ret;
}

int oplus_sy6970_disable_otg(void)
{
	int ret = 0;

	ret = sy6970_disable_otg(g_bq);

	if (ret < 0) {
		dev_notice(g_bq->dev, "%s disable otg fail(%d)\n", __func__, ret);
		return ret;
	}

	g_bq->otg_enable = false;
	return ret;

}

int oplus_sy6970_disable_te(void)
{
	return  sy6970_enable_term(g_bq, false);
}

int oplus_sy6970_get_chg_current_step(void)
{
	return SY6970_ICHG_LSB;
}

int oplus_sy6970_get_charger_type(void)
{
	struct oplus_chg_chip *g_oplus_chip = oplus_chg_get_chg_struct();

	if (!g_oplus_chip)
		return POWER_SUPPLY_TYPE_UNKNOWN;

	if (g_bq->oplus_chg_type != g_oplus_chip->charger_type && g_oplus_chip->usb_psy)
		power_supply_changed(g_oplus_chip->usb_psy);

	return g_bq->oplus_chg_type;
}

static int oplus_sy6970_charger_suspend(void)
{
	if (g_bq) {
		atomic_set(&g_bq->charger_suspended, 1);
		sy6970_enter_hiz_mode(g_bq);
	}

	if ((g_oplus_chip) && (g_oplus_chip->is_double_charger_support)) {
		g_oplus_chip->slave_charger_enable = false;
		g_oplus_chip->sub_chg_ops->charger_suspend();
	}
	printk("%s\n",__func__);
	return 0;
}

int oplus_sy6970_charger_unsuspend(void)
{
	if (g_bq) {
		atomic_set(&g_bq->charger_suspended, 0);
		sy6970_exit_hiz_mode(g_bq);
	}

	if ((g_oplus_chip != NULL) && (g_oplus_chip->is_double_charger_support) && (g_bq != NULL) && (!sy6970_is_usb(g_bq))) {
		g_oplus_chip->sub_chg_ops->charger_unsuspend();
	}
	printk("%s\n",__func__);
	return 0;
}

int oplus_sy6970_set_rechg_vol(int vol)
{
	return 0;
}

int oplus_sy6970_reset_charger(void)
{
	return 0;
}

bool oplus_sy6970_check_charger_resume(void)
{
	return true;
}

void oplus_sy6970_set_chargerid_switch_val(int value)
{
	return;
}

int oplus_sy6970_get_chargerid_switch_val(void)
{
	return 0;
}

int oplus_sy6970_get_chargerid_volt(void)
{
	return 0;
}

bool oplus_sy6970_check_chrdet_status(void)
{
	return g_bq->power_good;
}

static int oplus_sy6970_get_charger_subtype(void)
{
	if (oplus_sy6970_get_pd_type()) {
		return CHARGER_SUBTYPE_PD;
	}

	if ((sy6970_is_hvdcp(g_bq) || g_bq->hvdcp_can_enabled)
		&& (!disable_QC)) {
		return CHARGER_SUBTYPE_QC;
	} else {
		return CHARGER_SUBTYPE_DEFAULT;
	}
}

bool oplus_sy6970_need_to_check_ibatt(void)
{
	return false;
}

int oplus_sy6970_get_dyna_aicl_result(void)
{
	int mA = 0;

	sy6970_read_idpm_limit(g_bq, &mA);
	return mA;
}

#define CONVERT_RETRY_COUNT		100
#define SY6970_5V_THRES_MV		5800
#define SY6970_9V_THRES_MV		7500
#define SY6970_9V_THRES1_MV		7600
#define CONVERY_DELAY_MS		50
#define ADAPTER_33W_SUSPEND_ICHG	100
#define ADAPTER_33W_DELAY_MS		1500
#define CONVERT_MIN_ICHG_MA		500
void vol_convert_work(struct work_struct *work)
{
	int retry = CONVERT_RETRY_COUNT;
	u32 icharging = 0;

	if (!g_bq->pdqc_setup_5v) {
		if (oplus_sy6970_get_vbus() < SY6970_5V_THRES_MV) {
			/*protect the charger: enable ilim.*/
			sy6970_enable_enlim(g_bq);
			icharging = oplus_get_charger_current();
			if (icharging < CONVERT_MIN_ICHG_MA) {
				icharging = CONVERT_MIN_ICHG_MA;
			}

			/*Fix 11V3A oplus charger can't change to 9V after back to normal temperature.*/
			chg_info("wait charger respond");
			oplus_sy6970_set_ichg(ADAPTER_33W_SUSPEND_ICHG);
			msleep(ADAPTER_33W_DELAY_MS);

			sy6970_enable_hvdcp(g_bq);
			sy6970_switch_to_hvdcp(g_bq, HVDCP_9V);
			Charger_Detect_Init();
			oplus_for_cdp();
			g_bq->is_force_aicl = true;
			g_bq->is_retry_bc12 = true;
			sy6970_force_dpdm(g_bq, true);
			g_bq->hvdcp_checked = false;
			msleep(CONVERY_DELAY_MS);
			while(retry--) {
				if (oplus_sy6970_get_vbus() > SY6970_9V_THRES1_MV) {
					chg_info("set_to_9v success\n");
					break;
				}
				msleep(CONVERY_DELAY_MS);
			}
			chg_info("set_to_9v complete.\n");
			g_bq->is_force_aicl = false;
			sy6970_disable_enlim(g_bq);
			oplus_sy6970_set_ichg(icharging);
		} else {
			chg_err("set_to_9v already 9V.\n");
		}
	} else {
		if (oplus_sy6970_get_vbus() > SY6970_9V_THRES_MV) {
			/*protect the charger: enable ilim.*/
			sy6970_enable_enlim(g_bq);
			sy6970_disable_hvdcp(g_bq);
			Charger_Detect_Init();
			oplus_for_cdp();
			g_bq->is_force_aicl = true;
			g_bq->is_retry_bc12 = true;
			sy6970_force_dpdm(g_bq, true);
			msleep(CONVERY_DELAY_MS);
			while(retry--) {
				if (oplus_sy6970_get_vbus() < SY6970_5V_THRES_MV) {
					chg_info("set_to_5v success");
					break;
				}
				msleep(CONVERY_DELAY_MS);
			}
			chg_info("set_to_5v complete, set is_force_aicl as false.\n");
			g_bq->is_force_aicl = false;
			sy6970_disable_enlim(g_bq);
		} else {
			chg_err("set_to_5v already 5V.\n");
		}
	}
	g_bq->is_bc12_end = true;

	return;
}

#define COOL_DOWN_5V_UI_SOC_LEVEL 90
#define COOL_DOWN_5V_CHARGER_VOL 7500
#define PULL_UP_9V_CHARGER_VOL 6500
#define BLACK_COOL_DOWN_COUNT 3

bool oplus_chg_check_qchv_condition(void)
{
	struct oplus_chg_chip *chip = g_oplus_chip;

	if (!chip) {
		pr_err("oplus_chip is null\n");
		return false;
	}

	chip->charger_volt = chip->chg_ops->get_charger_volt();
	if (!chip->calling_on && !chip->camera_on && chip->charger_volt < PULL_UP_9V_CHARGER_VOL
	    && chip->soc < COOL_DOWN_5V_UI_SOC_LEVEL
	    && (chip->limits.tbatt_pdqc_to_9v_thr < 0
	    || chip->temperature <= chip->limits.tbatt_pdqc_to_9v_thr)
	    && !chip->cool_down_force_5v
	    && g_bq->qc_to_9v_count < BLACK_COOL_DOWN_COUNT) {
		return true;
	}

	return false;
}

int oplus_sy6970_set_qc_config(void)
{
	struct oplus_chg_chip *chip = g_oplus_chip;
	int ret = 0;

#ifdef CONFIG_OPLUS_CHARGER_MTK
	struct mtk_charger *info = NULL;
	if (g_bq->chg_consumer != NULL)
		info = g_bq->chg_consumer->cm;

	if (!info){
		dev_info(g_bq->dev, "%s:error\n", __func__);
		return -1;
	}
#endif

	if (!chip) {
		dev_info(g_bq->dev, "%s: error\n", __func__);
		return -1;
	}

	if (disable_QC){
		dev_info(g_bq->dev, "%s:disable_QC\n", __func__);
		return -1;
	}

	if (g_bq->disable_hight_vbus==1){
		dev_info(g_bq->dev, "%s:disable_hight_vbus\n", __func__);
		return -1;
	}

	if (chip->charging_state == CHARGING_STATUS_FAIL) {
		dev_info(g_bq->dev, "%s:charging_status_fail\n", __func__);
		return -1;
	}

	chg_info("temperature %d, tbatt_pdqc_to_5v_thr %d, charger_volt %d, batt_volt %d, ui_soc %d, \
		icharging %d, \
		vbatt_pdqc_to_5v_thr %d, \
		cool_down_force_5v %d, \
		tbatt_pdqc_to_9v_thr %d \n",
		chip->temperature,
		chip->limits.tbatt_pdqc_to_5v_thr,
		chip->charger_volt,
		chip->batt_volt,
		chip->ui_soc,
		chip->icharging,
		chip->limits.vbatt_pdqc_to_5v_thr,
		chip->cool_down_force_5v,
		chip->limits.tbatt_pdqc_to_9v_thr);
	if (chip->cool_down_force_5v
		|| ((chip->batt_volt > chip->limits.vbatt_pdqc_to_5v_thr) && (chip->ui_soc >= COOL_DOWN_5V_UI_SOC_LEVEL))
		|| ((chip->limits.tbatt_pdqc_to_5v_thr > 0) && (chip->temperature > chip->limits.tbatt_pdqc_to_5v_thr))) {
		if (oplus_sy6970_get_vbus() > COOL_DOWN_5V_CHARGER_VOL) {
			chg_info(" start set qc to 5V");
			g_bq->pdqc_setup_5v = true;
			g_bq->qc_to_9v_count = 0;
			if (g_bq->is_bc12_end) {
				g_bq->is_bc12_end = false;
				g_bq->qc_aicl_true = false;
				schedule_delayed_work(&g_bq->sy6970_vol_convert_work, 0);
			} else {
				ret = -1;
			}
		} else {
			ret = -1;
		}
	} else if (!chip->cool_down_force_5v
			&& (chip->batt_volt <= chip->limits.vbatt_pdqc_to_9v_thr)
			&& (chip->charging_state == CHARGING_STATUS_CCCV)
			&& (chip->limits.tbatt_pdqc_to_9v_thr < 0
			|| chip->temperature < chip->limits.tbatt_pdqc_to_9v_thr)
			&& (chip->ui_soc < COOL_DOWN_5V_UI_SOC_LEVEL)
			&& (oplus_sy6970_get_vbus() < PULL_UP_9V_CHARGER_VOL)) {
		chg_info("start set to 9V. count (%d)", g_bq->qc_to_9v_count);
		g_bq->pdqc_setup_5v = false;

		if (g_bq->is_bc12_end) {
			g_bq->is_bc12_end = false;
			g_bq->qc_aicl_true = false;
			schedule_delayed_work(&g_bq->sy6970_vol_convert_work, 0);
		} else {
			ret = -1;
		}
		g_bq->qc_to_9v_count++;
	} else {
		g_bq->qc_to_9v_count = 0;
		if (g_bq->qc_aicl_true) {
			chg_err("need do aicl once for qc");
			ret = 0;
		} else {
			ret = -1;
		}
	}

	return ret;
}

int oplus_sy6970_enable_qc_detect(void)
{
	return 0;
}

#ifdef CONFIG_OPLUS_SHORT_HW_CHECK
static bool oplus_shortc_check_is_gpio(struct oplus_chg_chip *chip)
{
	if (!chip) {
		printk(KERN_ERR "[OPLUS_CHG][%s]: oplus_chip not ready!\n", __func__);
		return false;
	}

	if (gpio_is_valid(chip->normalchg_gpio.shortc_gpio)) {
		return true;
	}

	return false;
}

static bool oplus_sy6970_get_shortc_hw_gpio_status(void)
{
	bool shortc_hw_status = true;
	struct oplus_chg_chip *chip = g_oplus_chip;
	if (!chip) {
		printk(KERN_ERR "[OPLUS_CHG][%s]: oplus_chip not ready!\n", __func__);
		return shortc_hw_status;
	}

	if (oplus_shortc_check_is_gpio(chip) == true) {
		shortc_hw_status = !!(gpio_get_value(chip->normalchg_gpio.shortc_gpio));
	}
	return shortc_hw_status;
}
#else /* CONFIG_OPLUS_SHORT_HW_CHECK */
static bool oplus_sy6970_get_shortc_hw_gpio_status(void)
{
	bool shortc_hw_status = true;
	return shortc_hw_status;
}
#endif /* CONFIG_OPLUS_SHORT_HW_CHECK */

#define SY6970_CHECK_TYPE_COUNT		75
#define SY6970_CHECK_TYPE_DELAY_MS	20
int oplus_charger_type_thread(void *data)
{
	int ret = 0;
	u8 reg_val = 0;
	int vbus_stat = 0;
	struct sy6970 *bq = (struct sy6970 *) data;
	int re_check_count = 0;

#ifdef CONFIG_OPLUS_CHARGER_MTK
	/* struct sched_param param = {.sched_priority = MAX_RT_PRIO-1}; */
	/* sched_setscheduler(current, SCHED_FIFO, &param); */
#endif
	while (1) {
		wait_event_interruptible(oplus_chgtype_wq,bq->chg_need_check == true);
		re_check_count = 0;
		bq->chg_start_check = true;
		bq->chg_need_check = false;
RECHECK:
		ret = sy6970_read_byte(bq, SY6970_REG_0B, &reg_val);
		if (ret)
			break;

		vbus_stat = (reg_val & SY6970_VBUS_STAT_MASK);
		vbus_stat >>= SY6970_VBUS_STAT_SHIFT;
		bq->vbus_type = vbus_stat;

		switch (vbus_stat) {
		case SY6970_VBUS_TYPE_NONE:
			bq->chg_type = CHARGER_UNKNOWN;
			bq->oplus_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
			oplus_set_usb_props_type(bq->oplus_chg_type);
			msleep(SY6970_CHECK_TYPE_DELAY_MS);
			re_check_count++;
			if(re_check_count == SY6970_CHECK_TYPE_COUNT) {
				bq->chg_type = CHARGER_UNKNOWN;
				goto RECHECK;
			} else if (bq->power_good) {
				goto RECHECK;
			}
		case SY6970_VBUS_TYPE_SDP:
			bq->chg_type = STANDARD_HOST;
			if (!bq->sdp_retry) {
				bq->sdp_retry = true;
				schedule_delayed_work(&g_bq->sy6970_retry_adapter_detection, OPLUS_BC12_RETRY_TIME);
			}
			break;
		case SY6970_VBUS_TYPE_CDP:
			bq->chg_type = CHARGING_HOST;
			if (!bq->cdp_retry) {
				bq->cdp_retry = true;
				schedule_delayed_work(&bq->sy6970_retry_adapter_detection, OPLUS_BC12_RETRY_TIME_CDP);
			}
			break;
		case SY6970_VBUS_TYPE_DCP:
			bq->chg_type = STANDARD_CHARGER;
			break;
		case SY6970_VBUS_TYPE_HVDCP:
			bq->chg_type = STANDARD_CHARGER;
			bq->hvdcp_can_enabled = true;
			break;
		case SY6970_VBUS_TYPE_UNKNOWN:
			bq->chg_type = NONSTANDARD_CHARGER;
			break;
		case SY6970_VBUS_TYPE_NON_STD:
			bq->chg_type = NONSTANDARD_CHARGER;
			break;
		default:
			bq->chg_type = NONSTANDARD_CHARGER;
			break;
		}
	}

	return 0;
}

static void charger_type_thread_init(void)
{
	g_bq->chg_need_check = false;

	charger_type_kthread = kthread_run(oplus_charger_type_thread, g_bq, "chgtype_kthread");
	if (IS_ERR(charger_type_kthread)) {
		chg_err("failed to cread oplus_usbtemp_kthread\n");
	}
}

static int oplus_sy6970_get_pd_type(void)
{
	if (disable_PD) {
		dev_info(g_bq->dev, "%s:disable_PD\n", __func__);
		return CHARGER_SUBTYPE_DEFAULT;
	}

	return oplus_sm8150_get_pd_type();
}

void sy6970_force_pd_to_dcp(void)
{
	g_bq->oplus_chg_type = POWER_SUPPLY_TYPE_USB_DCP;
	oplus_set_usb_props_type(g_bq->oplus_chg_type);
	g_bq->is_force_aicl = false;
}


#ifdef CONFIG_OPLUS_CHARGER_MTK
enum boot_reason_t get_boot_reason(void)
{
	return BR_UNKNOWN;
}

static void oplus_mt_power_off(void)
{
	if (!g_oplus_chip) {
		printk(KERN_ERR "[OPLUS_CHG][%s]: oplus_chip not ready!\n", __func__);
		return;
	}

	if (g_oplus_chip->ac_online != true) {
		if(!mt6357_get_vbus_status())
			kernel_power_off();
	} else {
		printk(KERN_ERR "[OPLUS_CHG][%s]: ac_online is true, return!\n", __func__);
	}
}
#endif

#define VBUS_VALID_MV	4000
static void sy6970_init_work_handler(struct work_struct *work)
{
	int boot_mode = get_boot_mode();
	u8 ret = 0;
	u8 reg_val = 0;

	chg_debug("boot_mode = %d", boot_mode);

#ifdef CONFIG_OPLUS_CHARGER_MTK
	if ((boot_mode != META_BOOT)
		&& (oplus_sy6970_get_vbus() > VBUS_VALID_MV)
		&& !g_oplus_chip->ac_online ) {
#else
	if (oplus_sy6970_get_vbus() > VBUS_VALID_MV) {
#endif
		chg_info("USB is inserted!");
		mutex_lock(&g_bq->chgdet_en_lock);

#ifdef CONFIG_TCPC_CLASS
		// if (tcpm_inquire_typec_attach_state(g_bq->tcpc) != TYPEC_ATTACHED_SRC)
#endif
		{
			ret = sy6970_read_byte(g_bq, SY6970_REG_0B, &reg_val);
			if (0 == ret) {
				g_bq->power_good = !!(reg_val & SY6970_PG_STAT_MASK);
			}

			chg_info("USB is inserted, power_good = %d !", g_bq->power_good);

			sy6970_chgdet_en(g_bq, true);
		}

		mutex_unlock(&g_bq->chgdet_en_lock);
	}
	return;
}
static u8 wait_pd = 0;
#define WAIT_PD_TIME_S 20
#define INPUT_CURRENT_MIN_MA 100
static void sy6970_hvdcp_bc12_work_handler(struct work_struct *work)
{
	unsigned long time = 0;
	u32 icl = 0;

	oplus_chg_get_curr_time_ms(&time);
	time = time / UNIT_TRANS_1000;

	if (!g_bq) {
		return;
	}

	if ((!oplus_check_pdphy_ready()) && (time < WAIT_PD_TIME_S)) {
		if (g_bq->power_good) {
			wait_pd = 1;
			oplus_set_usb_props_type(g_bq->oplus_chg_type);
			oplus_chg_wake_update_work();
			schedule_delayed_work(&g_bq->sy6970_hvdcp_bc12_work, msecs_to_jiffies(SY6970_HVDCP_BC12_WORK_DELAY));
		}
		return;
	}
	chg_info("start hvdcp bc1.2.");
	if (wait_pd) {
		wait_pd = 0;
		if (!oplus_pd_connected()) {
			chg_info("suspend hvdcp bc1.2. 1.5s");
			oplus_sy6970_get_icl(g_bq, &icl);
			oplus_sy6970_set_icl(g_bq, INPUT_CURRENT_MIN_MA);
			msleep(SY6970_HVDCP_BC12_WORK_DELAY);
			oplus_sy6970_set_icl(g_bq, icl);
		}
	}
	sy6970_enable_enlim(g_bq);
	g_bq->hvdcp_checked = true;
	sy6970_enable_hvdcp(g_bq);
	sy6970_switch_to_hvdcp(g_bq, HVDCP_9V);
	Charger_Detect_Init();
	oplus_for_cdp();
	sy6970_force_dpdm(g_bq, true);
	sy6970_enable_auto_dpdm(g_bq,false);
	return;
}

int oplus_sy6970_get_vbus(void)
{
	int chg_vol = 3000;

	if (g_bq) {
		chg_vol = sy6970_adc_read_vbus_volt(g_bq);
	}
	return chg_vol;
}

static int oplus_sy6970_set_pdo_5v(void)
{
	if (!g_bq)
		return 0;

	chg_err("set hvdcp_can_enabled as false and disable hvdcp");
	g_bq->hvdcp_can_enabled = false;
	sy6970_disable_hvdcp(g_bq);

	return 0;
}

struct oplus_chg_operations  oplus_chg_sy6970_ops = {
	.dump_registers = oplus_sy6970_dump_registers,
	.kick_wdt = oplus_sy6970_kick_wdt,
	.hardware_init = oplus_sy6970_hardware_init,
	.charging_current_write_fast = oplus_sy6970_set_ichg,
	.set_aicl_point = oplus_sy6970_set_mivr,
	.input_current_write = oplus_sy6970_set_input_current_limit,
	.float_voltage_write = oplus_sy6970_set_cv,
	.term_current_set = oplus_sy6970_set_ieoc,
	.charging_enable = oplus_sy6970_charging_enable,
	.charging_disable = oplus_sy6970_charging_disable,
	.get_charging_enable = oplus_sy6970_is_charging_enabled,
	.charger_suspend = oplus_sy6970_charger_suspend,
	.charger_unsuspend = oplus_sy6970_charger_unsuspend,
	.set_rechg_vol = oplus_sy6970_set_rechg_vol,
	.reset_charger = oplus_sy6970_reset_charger,
	.read_full = oplus_sy6970_is_charging_done,
	.otg_enable = oplus_sy6970_enable_otg,
	.otg_disable = oplus_sy6970_disable_otg,
	.set_charging_term_disable = oplus_sy6970_disable_te,
	.check_charger_resume = oplus_sy6970_check_charger_resume,
	.get_charger_type = oplus_sy6970_get_charger_type,
	.get_boot_mode = get_boot_mode,
	.check_chrdet_status = oplus_sy6970_check_chrdet_status,
	.set_chargerid_switch_val = oplus_sy6970_set_chargerid_switch_val,
        .get_chargerid_switch_val = oplus_sy6970_get_chargerid_switch_val,
#ifdef CONFIG_OPLUS_CHARGER_MTK
	.get_charger_volt = mt6357_get_vbus_voltage,
	.get_chargerid_volt = oplus_sy6970_get_chargerid_volt,
	.get_boot_reason = (int (*)(void))get_boot_reason,
	.get_instant_vbatt = oplus_battery_meter_get_battery_voltage,
	.get_rtc_soc = oplus_get_rtc_ui_soc,
	.set_rtc_soc = oplus_set_rtc_ui_soc,
	.set_power_off = oplus_mt_power_off,
	.usb_connect = mt_usb_connect,
	.usb_disconnect = mt_usb_disconnect,
#else
	.get_chargerid_volt = NULL,
	.get_charger_volt = qpnp_get_prop_charger_voltage_now,
	.get_boot_reason = smbchg_get_boot_reason,
	.get_instant_vbatt = qpnp_get_battery_voltage,
	.get_rtc_soc = oplus_chg_get_shutdown_soc,
	.set_rtc_soc = oplus_chg_backup_soc,
	.set_power_off = NULL,
        .usb_connect = NULL,
        .usb_disconnect = NULL,
#endif
	.get_chg_current_step = oplus_sy6970_get_chg_current_step,
	.need_to_check_ibatt = oplus_sy6970_need_to_check_ibatt,
	.get_dyna_aicl_result = oplus_sy6970_get_dyna_aicl_result,
	.get_shortc_hw_gpio_status = oplus_sy6970_get_shortc_hw_gpio_status,
	.oplus_chg_get_pd_type = oplus_sy6970_get_pd_type,
	.oplus_chg_pd_setup = oplus_chg_set_pd_config,
	.get_charger_subtype = oplus_sy6970_get_charger_subtype,
	.set_qc_config = oplus_sy6970_set_qc_config,
	.enable_qc_detect = oplus_sy6970_enable_qc_detect,
	.oplus_chg_set_high_vbus = NULL,
	.enable_shipmode = sy6970_enable_shipmode,
	.oplus_chg_set_hz_mode = sy6970_set_hz_mode,
	.oplus_usbtemp_monitor_condition = oplus_usbtemp_condition,
	.get_usbtemp_volt = oplus_get_usbtemp_volt,
	.really_suspend_charger = sy6970_really_suspend_charger,
	.set_typec_cc_open = oplus_set_typec_cc_open,
	.set_typec_sinkonly = oplus_set_typec_sinkonly,
	.force_pd_to_dcp = sy6970_force_pd_to_dcp,
	.check_qchv_condition = oplus_chg_check_qchv_condition,
	.pdo_5v = oplus_sy6970_set_pdo_5v,
	.subcharger_force_enable = oplus_sy6970_subcharger_force_enable,
};

static void retry_detection_work_callback(struct work_struct *work)
{
	if (g_bq->sdp_retry || g_bq->cdp_retry || g_bq->retry_hvdcp_algo) {
		Charger_Detect_Init();
		chg_info("usb/cdp start bc1.2 once");
		oplus_for_cdp();
		g_bq->usb_connect_start = true;
		g_bq->is_force_aicl = true;
		g_bq->is_retry_bc12 = true;
		sy6970_force_dpdm(g_bq, true);
	}
}

#define AICL_RECHECK_COUNT	200
#define AICL_RECHECK_DELAY_MS	20
static void aicr_setting_work_callback(struct work_struct *work)
{
	int re_check_count = 0;

	if (!g_bq)
                return;

	chg_debug("start.");
	if (g_bq->oplus_chg_type == POWER_SUPPLY_TYPE_USB_DCP && g_bq->is_force_aicl) {
		while (g_bq->is_force_aicl) {
			if (re_check_count++ < AICL_RECHECK_COUNT) {
				msleep(AICL_RECHECK_DELAY_MS);
			} else {
				break;
			}
		}
	}
	chg_debug("begin aicr current = %d mA, wait %d mS", g_bq->aicr, re_check_count*AICL_RECHECK_DELAY_MS);
	oplus_sy6970_set_aicr(g_bq->aicr);
	return;
}

int oplus_sy6970_set_ichg(int cur)
{
	struct oplus_chg_chip *chip = g_oplus_chip;
	int uA = 0;
	int main_uA = 0;
	int slave_uA = 0;
	int ret = 0;
	int temp = 0;

	chg_debug(" current = %d mA", cur);

	if (!chip) {
		return 0;
	}

	if (!g_bq) {
		return 0;
	}

	if (atomic_read(&g_bq->charger_suspended) == 1) {
		chg_err("suspend,ignore set cur = %d mA\n", cur);
		return 0;
	}

	if (cur > BQ_CHARGER_CURRENT_MAX_MA) {
		cur = BQ_CHARGER_CURRENT_MAX_MA;
	}

	uA = cur * UNIT_TRANS_1000;
	if (chip->is_double_charger_support) {
		if (chip->slave_charger_enable || chip->em_mode ) {
			main_uA = uA * (FULL_PCT-chip->slave_pct) / FULL_PCT;
			ret = sy6970_set_chargecurrent(g_bq, main_uA/UNIT_TRANS_1000);
			if ( !ret ) {
				ret = _sy6970_get_ichg(g_bq, &main_uA);
			} else {
				chg_err("set main_uA failed.");
			}
			slave_uA = uA - main_uA;
			if (chip->sub_chg_ops) {
				chg_debug("set main_uA = %d, slave_uA = %d", main_uA, slave_uA);

				/*slave charger: set ichg */
				temp = slave_uA/UNIT_TRANS_1000%64;
				if (temp > 32) {
					slave_uA = slave_uA + (64- temp)*UNIT_TRANS_1000;
				}

				if (chip->sub_chg_ops->charging_current_write_fast) {
					ret = chip->sub_chg_ops->charging_current_write_fast(slave_uA/UNIT_TRANS_1000);
					if (ret) {
						chg_err("set slave_uA:%d failed", slave_uA);
					}
				}
			}
		} else {
			ret = sy6970_set_chargecurrent(g_bq, uA/UNIT_TRANS_1000);
		}
	} else {
		ret = sy6970_set_chargecurrent(g_bq, uA/UNIT_TRANS_1000);
		if (ret) {
			chg_err("set main uA %d failed", uA);
		}
	}

	chg_info(" current = %d, slave_pct=%d uA = %d, main_uA = %d, slave_uA = %d, slave_charger_enable) = %d\n",
		cur, chip->slave_pct, uA, main_uA, slave_uA, chip->slave_charger_enable);
	return 0;
}

static struct of_device_id sy6970_charger_match_table[] = {
	{.compatible = "ti,bq25890h",},
	{.compatible = "oplus,sy6970",},
	{},
};

MODULE_DEVICE_TABLE(of, sy6970_charger_match_table);

static const struct i2c_device_id sy6970_i2c_device_id[] = {
	{ "bq25890h", 0x03 },
	{ "sy6970", 0x01 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, sy6970_i2c_device_id);

static int sy6970_charger_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct sy6970 *bq;
	struct device_node *node = client->dev.of_node;
	int ret = 0;

	chg_debug("sy6970 probe enter");
	bq = devm_kzalloc(&client->dev, sizeof(struct sy6970), GFP_KERNEL);
	if (!bq)
		return -ENOMEM;

	bq->dev = &client->dev;
	bq->client = client;
	g_bq = bq;

	i2c_set_clientdata(client, bq);
	mutex_init(&bq->i2c_rw_lock);
	mutex_init(&bq->chgdet_en_lock);
	mutex_init(&bq->dpdm_lock);

	if (!bq->dpdm_reg && of_get_property(bq->dev->of_node,
				"dpdm-supply", NULL)) {
		bq->dpdm_reg = devm_regulator_get(bq->dev, "dpdm");
		if (IS_ERR(bq->dpdm_reg)) {
			ret = PTR_ERR(bq->dpdm_reg);
			chg_err("Couldn't get dpdm regulator ret=%d\n", ret);
			bq->dpdm_reg = NULL;
			goto err_parse_dt;
		}
	}

	ret = sy6970_detect_device(bq);
	if (ret) {
		pr_err("No sy6970 device found!\n");
		ret = -ENODEV;
		goto err_nodev;
	}

	bq->platform_data = sy6970_parse_dt(node, bq);
	if (!bq->platform_data) {
		pr_err("No platform data provided.\n");
		ret = -EINVAL;
		goto err_parse_dt;
	}

	sy6970_reset_chip(bq);

	ret = sy6970_init_device(bq);
	if (ret) {
		pr_err("Failed to init device\n");
		goto err_init;
	}

	charger_type_thread_init();
	atomic_set(&bq->charger_suspended, 0);
	atomic_set(&bq->charger_suspended, 0);
	bq->is_bc12_end = true;

	bq->oplus_chg_type = POWER_SUPPLY_TYPE_UNKNOWN;
	oplus_set_usb_props_type(bq->oplus_chg_type);
	bq->pre_current_ma = -1;
	bq->chg_start_check = true;
	bq->usb_connect_start = false;
	bq->hvdcp_can_enabled = false;
	bq->sdp_retry = false;
	bq->cdp_retry = false;
	bq->qc_to_9v_count = 0;
	bq->qc_aicl_true = false;

	/* Charger_Detect_Init(); */
	sy6970_disable_hvdcp(bq);
	sy6970_disable_maxc(bq);
	sy6970_disable_batfet_rst(bq);
	/*Enable AICL for sy6970*/
	sy6970_enable_ico(bq, true);

	INIT_DELAYED_WORK(&bq->sy6970_aicr_setting_work, aicr_setting_work_callback);
	INIT_DELAYED_WORK(&bq->sy6970_vol_convert_work, vol_convert_work);
	INIT_DELAYED_WORK(&bq->sy6970_retry_adapter_detection, retry_detection_work_callback);
	INIT_DELAYED_WORK(&bq->init_work, sy6970_init_work_handler);
	INIT_DELAYED_WORK(&bq->sy6970_hvdcp_bc12_work, sy6970_hvdcp_bc12_work_handler);

	ret = sy6970_register_interrupt(node, bq);
	if (ret) {
		chg_err("Failed to register irq ret=%d\n", ret);
		goto err_irq;
	}

#ifdef CONFIG_OPLUS_CHARGER_MTK
	bq->chg_dev = charger_device_register(bq->chg_dev_name,
					      &client->dev, bq,
					      &sy6970_chg_ops,
					      &sy6970_chg_props);
	if (IS_ERR_OR_NULL(bq->chg_dev)) {
		ret = PTR_ERR(bq->chg_dev);
		goto err_device_register;
	}

	if (ret){
		dev_err(bq->dev, "failed to register sysfs. err: %d\n", ret);
		goto err_sysfs_create;
	}
#endif

	determine_initial_status(bq);

#ifdef CONFIG_OPLUS_CHARGER_MTK
        bq->boot_mode = get_boot_mode();
        if (bq->boot_mode == META_BOOT) {
		chg_info("disable_charger for meta boot.\n");
		sy6970_enter_hiz_mode(bq);
		/* sy6970_disable_charger(bq);
		INIT_DELAYED_WORK(&bq->enter_hz_work, sy6970_enter_hz_work_handler); */

		/* Stop charging for META BOOT after the ELT&ETS ports is enable */
		/* schedule_delayed_work(&bq->enter_hz_work, 0); */
        }
#else
        if (oplus_is_rf_ftm_mode()) {
		chg_info(" disable_charger for ftm mode.\n");
		sy6970_enter_hiz_mode(bq);
		/* sy6970_disable_charger(bq);

		INIT_DELAYED_WORK(&bq->enter_hz_work, sy6970_enter_hz_work_handler);
		schedule_delayed_work(&bq->enter_hz_work, msecs_to_jiffies(10000)); */
        }
#endif
	if (strcmp(bq->chg_dev_name, "primary_chg") == 0) {
		/*
		* The init_work of bc1.2 shall be after the usbphy is ready,
		* otherwise the adb will not work when reboot with usb plug in.
		*/
		bq->boot_mode = get_boot_mode();
#ifdef CONFIG_OPLUS_CHARGER_MTK
		if (NORMAL_BOOT == bq->boot_mode)
#else
		if (MSM_BOOT_MODE__NORMAL == bq->boot_mode)
#endif
			schedule_delayed_work(&bq->init_work, msecs_to_jiffies(INIT_WORK_NORMAL_DELAY));
		else
			schedule_delayed_work(&bq->init_work, msecs_to_jiffies(INIT_WORK_OTHER_DELAY));
	}

	set_charger_ic(SY6970);
	chg_err("sy6970 probe success, Part Num:%d, Revision:%d\n",
		bq->part_no, bq->revision);

	return 0;

#ifdef CONFIG_OPLUS_CHARGER_MTK
err_sysfs_create:
	charger_device_unregister(bq->chg_dev);
err_device_register:
#endif

err_irq:
err_init:
err_parse_dt:
err_nodev:
	mutex_destroy(&bq->i2c_rw_lock);
	mutex_destroy(&bq->chgdet_en_lock);
	devm_kfree(bq->dev, bq);
	return ret;

}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0))
static int sy6970_pm_resume(struct device *dev)
{
	struct sy6970 *chip = NULL;
        struct i2c_client *client = to_i2c_client(dev);

	chg_err(" suspend stop \n");
        if (client) {
                chip = i2c_get_clientdata(client);
                if (chip) {
			chg_err(" set charger_suspended as 0\n");
                        atomic_set(&chip->charger_suspended, 0);
		}
        }

        return 0;
}

static int sy6970_pm_suspend(struct device *dev)
{
        struct sy6970 *chip = NULL;
        struct i2c_client *client = to_i2c_client(dev);

	chg_err(" suspend start \n");
	if (client) {
                chip = i2c_get_clientdata(client);
                if (chip) {
			chg_err(" set charger_suspended as 1\n");
                        atomic_set(&chip->charger_suspended, 1);
		}
        }

        return 0;
}

static const struct dev_pm_ops sy6970_pm_ops = {
        .resume                 = sy6970_pm_resume,
        .suspend                = sy6970_pm_suspend,
};
#else
static int sy6970_resume(struct i2c_client *client)
{
       	struct sy6970 *chip = i2c_get_clientdata(client);

        if (!chip) {
                return 0;
        }

        atomic_set(&chip->charger_suspended, 0);

        return 0;
}

static int sy6970_suspend(struct i2c_client *client, pm_message_t mesg)
{
        struct sy6970 *chip = i2c_get_clientdata(client);

        if (!chip) {
                return 0;
        }

        atomic_set(&chip->charger_suspended, 1);

        return 0;
}
#endif


static int sy6970_charger_remove(struct i2c_client *client)
{
	struct sy6970 *bq = i2c_get_clientdata(client);

	mutex_destroy(&bq->i2c_rw_lock);
	mutex_destroy(&bq->chgdet_en_lock);

	sysfs_remove_group(&bq->dev->kobj, &sy6970_attr_group);

	return 0;
}

static void sy6970_charger_shutdown(struct i2c_client *client)
{
	if ((g_oplus_chip != NULL) && g_bq != NULL) {
		if((g_bq->hvdcp_can_enabled) && (g_oplus_chip->charger_exist)) {
			sy6970_disable_hvdcp(g_bq);
			sy6970_force_dpdm(g_bq, true);
			oplus_sy6970_charging_disable();
		}
	}
}

static struct i2c_driver sy6970_charger_driver = {
	.driver = {
		   .name = "sy6970-charger",
		   .owner = THIS_MODULE,
		   .of_match_table = sy6970_charger_match_table,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0))
		   .pm   = &sy6970_pm_ops,
#endif
		   },

	.probe = sy6970_charger_probe,
	.remove = sy6970_charger_remove,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0))
        .resume         = sy6970_resume,
        .suspend        = sy6970_suspend,
#endif
	.shutdown = sy6970_charger_shutdown,
	.id_table = sy6970_i2c_device_id,

};

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0))
module_i2c_driver(sy6970_charger_driver);
#else
void sy6970_charger_exit(void)
{
        i2c_del_driver(&sy6970_charger_driver);
}

int sy6970_charger_init(void)
{
        int ret = 0;
        chg_debug(" init start\n");

        oplus_chg_ops_register("ext-sy6970", &oplus_chg_sy6970_ops);
        if (i2c_add_driver(&sy6970_charger_driver) != 0) {
                chg_err(" failed to register sy6970 i2c driver.\n");
        } else {
                chg_debug(" Success to register sy6970 i2c driver.\n");
        }
        return ret;
}
#endif

MODULE_DESCRIPTION("TI SY6970 Charger Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Texas Instruments");
