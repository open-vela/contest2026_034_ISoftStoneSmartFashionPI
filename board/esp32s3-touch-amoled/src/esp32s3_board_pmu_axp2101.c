#include <nuttx/config.h>

#include <stdio.h>
#include <stdbool.h>
#include <debug.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/power/battery_charger.h>
#include <nuttx/power/battery_ioctl.h>
#include <nuttx/power/axp2101.h>

#include <arch/board/board.h>

#include "esp32s3_gpio.h"
#include "esp32s3_i2c.h"
#include "esp32s3_board_i2c.h"
#include "esp32s3-touch-amoled.h"

/* AXP2101 电源路径相关寄存器位定义（本地，不修改 nuttx 主线头文件） */
#define AXP2101_BATFET_CTRL                           (0x12)
#define AXP2101_BATFET_CTRL_POWEROFF_ENABLE           (1 << 3)
#define AXP2101_BATFET_CTRL_OCP_ENABLE                (1 << 1)

#define AXP2101_MIN_SYSTEM_VOLTAGE_SHIFT              (4)
#define AXP2101_MIN_SYSTEM_VOLTAGE_MASK               (0x07 << AXP2101_MIN_SYSTEM_VOLTAGE_SHIFT)
#define AXP2101_MIN_SYSTEM_VOLTAGE_4V4                (0x03 << AXP2101_MIN_SYSTEM_VOLTAGE_SHIFT)

#define AXP2101_INPUT_VOL_LIMIT_MASK                  (0x0f)
#define AXP2101_INPUT_VOL_LIMIT_4V36                  (0x06)

#define AXP2101_INPUT_CUR_LIMIT_MASK                  (0x07)
#define AXP2101_INPUT_CUR_LIMIT_1000MA                (0x03)

#define AXP2101_VOFF_SET                              (0x24)
#define AXP2101_VOFF_SET_MASK                         (0x07)
#define AXP2101_VOFF_SET_2V6                          (0x00)

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define WATCH_DBG_LOG(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)

static struct i2c_master_s *i2c;
static struct battery_charger_dev_s *g_axp2101_dev = NULL;

static int axp2101_reg_read(FAR struct i2c_master_s *i2c, uint8_t regaddr,
                            FAR uint8_t *regval)
{
  struct i2c_config_s config;
  int                 ret;

  config.frequency = AXP2101_FREQ;
  config.address   = AXP2101_ADDR;
  config.addrlen   = 7;

  ret = i2c_write(i2c, &config, &regaddr, sizeof(regaddr));
  if (ret < 0)
    {
      WATCH_DBG_LOG("AXP2101 i2c_write failed: %d", ret);
      return ret;
    }

  ret = i2c_read(i2c, &config, regval, sizeof(*regval));
  if (ret < 0)
    {
      WATCH_DBG_LOG("AXP2101 i2c_read failed: %d", ret);
      return ret;
    }

  return OK;
}

static int axp2101_reg_write(FAR struct i2c_master_s *i2c, uint8_t regaddr,
                             uint8_t regval)
{
  struct i2c_config_s config;
  uint8_t             buffer[2];
  int                 ret;

  buffer[0] = regaddr;
  buffer[1] = regval;

  config.frequency = AXP2101_FREQ;
  config.address   = AXP2101_ADDR;
  config.addrlen   = 7;

  ret = i2c_write(i2c, &config, buffer, sizeof(buffer));
  if (ret < 0)
    {
      WATCH_DBG_LOG("AXP2101 i2c_write failed: %d", ret);
      return ret;
    }

  return OK;
}

static int board_axp2101_powerpath_init(void)
{
  int     ret;
  uint8_t val;

  /* 0x12 BATFET_CTRL: 使能 POWEROFF 状态下电池供电 + 过流保护 */
  ret = axp2101_reg_read(i2c, AXP2101_BATFET_CTRL, &val);
  if (ret != OK)
    {
      return ret;
    }

  val |= AXP2101_BATFET_CTRL_POWEROFF_ENABLE;
  val |= AXP2101_BATFET_CTRL_OCP_ENABLE;
  ret = axp2101_reg_write(i2c, AXP2101_BATFET_CTRL, val);
  if (ret != OK)
    {
      return ret;
    }

  /* 0x14 MIN_SYS_VOL_CTRL: VSYS DPM 设为 4.4V，让电池提前进入 supplement 状态 */
  ret = axp2101_reg_read(i2c, AXP2101_MIN_SYSTEM_VOLTAGE_CONTROL, &val);
  if (ret != OK)
    {
      return ret;
    }

  val &= ~AXP2101_MIN_SYSTEM_VOLTAGE_MASK;
  val |= AXP2101_MIN_SYSTEM_VOLTAGE_4V4;
  ret = axp2101_reg_write(i2c, AXP2101_MIN_SYSTEM_VOLTAGE_CONTROL, val);
  if (ret != OK)
    {
      return ret;
    }

  /* 0x15 INPUT_VOL_LIMIT_CTRL: VBUS 输入电压限制设为 4.36V */
  ret = axp2101_reg_read(i2c, AXP2101_INPUT_VOL_LIMIT_CTRL, &val);
  if (ret != OK)
    {
      return ret;
    }

  val &= ~AXP2101_INPUT_VOL_LIMIT_MASK;
  val |= AXP2101_INPUT_VOL_LIMIT_4V36;
  ret = axp2101_reg_write(i2c, AXP2101_INPUT_VOL_LIMIT_CTRL, val);
  if (ret != OK)
    {
      return ret;
    }

  /* 0x16 INPUT_CUR_LIMIT_CTRL: VBUS 输入电流限制设为 1A */
  ret = axp2101_reg_read(i2c, AXP2101_INPUT_CUR_LIMIT_CTRL, &val);
  if (ret != OK)
    {
      return ret;
    }

  val &= ~AXP2101_INPUT_CUR_LIMIT_MASK;
  val |= AXP2101_INPUT_CUR_LIMIT_1000MA;
  ret = axp2101_reg_write(i2c, AXP2101_INPUT_CUR_LIMIT_CTRL, val);
  if (ret != OK)
    {
      return ret;
    }

  /* 0x24 VOFF_SET: 系统掉电阈值保持 2.6V，给电池最大余量 */
  ret = axp2101_reg_read(i2c, AXP2101_VOFF_SET, &val);
  if (ret != OK)
    {
      return ret;
    }

  val &= ~AXP2101_VOFF_SET_MASK;
  val |= AXP2101_VOFF_SET_2V6;
  ret = axp2101_reg_write(i2c, AXP2101_VOFF_SET, val);
  if (ret != OK)
    {
      return ret;
    }

  return OK;
}

int board_axp2101_initialize(void)
{
  int ret;
  struct battery_charger_dev_s * axp2101_dev;
  i2c = esp32s3_i2cbus_initialize(AXP2101_I2C);
  if (!i2c)
  {
    WATCH_DBG_LOG("ERROR: Failed to initialize i2c port %d", AXP2101_I2C);
    return -ENODEV;
  }

  axp2101_dev = axp2101_initialize(i2c, AXP2101_ADDR, AXP2101_FREQ);
  if (!axp2101_dev)
  {
    WATCH_DBG_LOG("ERROR: Failed to init axp2101 port %d", AXP2101_I2C);
    return -ENODEV;
  }

  /* 补充 AXP2101 电源路径配置，保证 VBUS 拔插时电池能平滑接管供电。
   * 部分机器因元件离散性，默认寄存器状态在拔 USB 瞬间会导致 VSYS
   * 跌落触发 PMU 关机，必须重新插拔电池才能恢复。
   */
  ret = board_axp2101_powerpath_init();
  if (ret != OK)
  {
    WATCH_DBG_LOG("ERROR: Failed to init axp2101 powerpath: %d", ret);
  }

  /* Register the battery charger device */
  ret = battery_charger_register("/dev/bat0", axp2101_dev);
  if (ret < 0)
  {
    WATCH_DBG_LOG("ERROR: Failed to register battery charger: %d", ret);
    return ret;
  }

  g_axp2101_dev = axp2101_dev;
  WATCH_DBG_LOG("AXP2101 battery charger successfully initialized at /dev/bat0");

  return OK;
}

struct i2c_master_s* get_i2c_from_init(void)
{
    return i2c;
}

struct battery_charger_dev_s* get_axp2101_dev(void)
{
    return g_axp2101_dev;
}
