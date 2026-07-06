#include <nuttx/config.h>

#include <stdio.h>
#include <stdbool.h>
#include <debug.h>
#include <errno.h>

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

static struct i2c_master_s *i2c;
static struct battery_charger_dev_s *g_axp2101_dev = NULL;

int board_axp2101_initialize(void)
{
  int ret;
  struct battery_charger_dev_s * axp2101_dev;
  i2c = esp32s3_i2cbus_initialize(AXP2101_I2C);
  if (!i2c)
  {
    printf("ERROR: Failed to initialize i2c port %d\n", AXP2101_I2C);
    return -ENODEV;
  }

  axp2101_dev = axp2101_initialize(i2c, AXP2101_ADDR, AXP2101_FREQ);
  if (!axp2101_dev)
  {
    printf("ERROR: Failed to init axp2101 port %d\n", AXP2101_I2C);
    return -ENODEV;
  }

  /* Register the battery charger device */
  ret = battery_charger_register("/dev/bat0", axp2101_dev);
  if (ret < 0)
  {
    printf("ERROR: Failed to register battery charger: %d\n", ret);
    return ret;
  }

  g_axp2101_dev = axp2101_dev;
  printf("AXP2101 battery charger successfully initialized at /dev/bat0\n");

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
