/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-touch-amoled/src/esp32s3-touch-amoled.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __BOARDS_XTENSA_ESP32S3_ESP32S3_TOUCH_AMOLED_SRC_ESP32S3_TOUCH_AMOLED_H
#define __BOARDS_XTENSA_ESP32S3_ESP32S3_TOUCH_AMOLED_SRC_ESP32S3_TOUCH_AMOLED_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <stdint.h>


struct i2c_master_s;
struct i2s_dev_s;


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ESP32-S3-TOUCH-AMOLED GPIOs *******************************************************/

/* BOOT Button */
#define BUTTON_BOOT  0

/* Display */
#define ESP32S3_TOUCH_AMOLED_DISPLAY_SPI         2
#define ESP32S3_TOUCH_AMOLED_DISPLAY_DC          43
#define ESP32S3_TOUCH_AMOLED_DISPLAY_BCKL        48

/* speaker ES8311 */
#ifdef CONFIG_AUDIO_ES8311
#define SPEAKER_ENABLE_GPIO      (46)
#define ES8311_I2C_ADDR          (0x18)
#define ES8311_I2C_FREQ          (100000)
#endif

/* mic ES7210 */
#ifdef CONFIG_AUDIO_ES7210
#define ES7210_I2C_ADDR     (0x40)
#define ES7210_I2C_FREQ     (100000)
#endif

/* touchscreen FT3168 */
#ifdef CONFIG_ESP32S3_WATCH_TOUCHSCREEN_FT3168
#define TOUCHSCEEN_CLOCK    (400 * 1000)
#define TOUCHSCEEN_I2C      (0)
#endif

/* sensor QMI8658 */
#ifdef CONFIG_ESP32S3_WATCH_SENSOR_QMI8658
#define QMI8658_I2C_PORT    (0)
#define QMI8658_I2C_ADDR    (0x6B)
#endif

/* pmu AXP2101 */
#ifdef CONFIG_AXP2101
#define AXP2101_ADDR       0x34
#define AXP2101_I2C        0
#define AXP2101_FREQ       100000
//#define AXP2101_INT        0     //实际电路该管脚没有接esp32s3的任何管脚
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_bringup
 *
 * Description:
 *   Perform architecture-specific initialization
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y && CONFIG_BOARDCTL=y :
 *     Called from the NSH library via board_app_initialize()
 *
 ****************************************************************************/

int esp32s3_bringup(void);

/****************************************************************************
 * Name: esp32s3_gpio_init
 *
 * Description:
 *   Configure the GPIO driver.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_DEV_GPIO
int esp32s3_gpio_init(void);
#endif

/****************************************************************************
 * Name: board_spiflash_init
 *
 * Description:
 *   Initialize the SPIFLASH and register the MTD device.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_SPIFLASH
int board_spiflash_init(void);
#endif


#ifdef CONFIG_ESP32S3_WATCH_SENSOR_QMI8658
int esp32s3_qmi8658_initialize(void);
#endif

#ifdef CONFIG_ESP32S3_WATCH_TOUCHSCREEN_FT3168
int board_touchscreen_initialize(void);
void ft3168_poll_pause(void);
void ft3168_poll_resume(void);
#endif

#ifdef CONFIG_ESP32S3_WATCH_AMOLED_CO5300
int board_amoled_initialize(void);
#endif

/****************************************************************************
 * Name: board_lcd_getdev
 *
 * Description:
 *   Return a reference to the LCD object for the specified LCD. This allows
 *   support for multiple LCD devices.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_WATCH_AMOLED_CO5300
struct lcd_dev_s *board_lcd_getdev(int lcddev);
#endif

/****************************************************************************
 * Name: board_lcd_uninitialize
 *
 * Description:
 *   Uninitialize the LCD support.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32S3_WATCH_AMOLED_CO5300
void board_lcd_uninitialize(void);
#endif

/****************************************************************************
 * Name: board_i2c_init
 *
 * Description:
 *   Configure the I2C driver.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_I2C_DRIVER
int board_i2c_init(void);
#endif

/****************************************************************************
 * Name: esp32s3_es8311_initialize
 *
 * Description:
 *   This function is called by platform-specific, setup logic to configure
 *   and register the ES8311 device.  This function will register the driver
 *   as /dev/audio/pcm[x] where x is determined by the I2S port number.
 *
 * Input Parameters:
 *   i2c_port  - The I2C port used for the device
 *   i2c_addr  - The I2C address used by the device
 *   i2c_freq  - The I2C frequency used for the device
 *   i2s_port  - The I2S port used for the device
 *
 * Returned Value:
 *   Zero is returned on success.  Otherwise, a negated errno value is
 *   returned to indicate the nature of the failure.
 *
 ****************************************************************************/
#ifdef CONFIG_AUDIO_ES8311
int esp32s3_es8311_initialize(struct i2c_master_s *i2c, struct i2s_dev_s *i2s);
#endif


#ifdef CONFIG_AUDIO_ES7210
int esp32s3_es7210_initialize(struct i2c_master_s *i2c, struct i2s_dev_s *i2s);
#endif

/****************************************************************************
 * Name: board_i2sdev_initialize
 *
 * Description:
 *   This function is called by platform-specific, setup logic to configure
 *   and register the generic I2S audio driver.  This function will register
 *   the driver as /dev/audio/pcm[x] where x is determined by the I2S port
 *   number.
 *
 * Input Parameters:
 *   port       - The I2S port used for the device
 *   enable_tx  - Register device as TX if true
 *   enable_rx  - Register device as RX if true
 *
 * Returned Value:
 *   Zero is returned on success.  Otherwise, a negated errno value is
 *   returned to indicate the nature of the failure.
 *
 ****************************************************************************/
#ifdef CONFIG_ESP32S3_I2S
int board_i2sdev_initialize(int port, bool enable_tx, bool enable_rx);
#endif


#ifdef CONFIG_AXP2101
int board_axp2101_initialize(void);
struct i2c_master_s* get_i2c_from_init(void);
struct battery_charger_dev_s* get_axp2101_dev(void);
#endif

/****************************************************************************
 * Watch Audio Interface
 ****************************************************************************/

#if defined(CONFIG_ESP32S3_I2S) && defined(CONFIG_AUDIO_ES8311)

int esp32s3_watch_audio_setvolume(uint16_t volume);
int esp32s3_watch_audio_getvolume(uint16_t *volume);

#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_XTENSA_ESP32S3_ESP32S3_TOUCH_AMOLED_SRC_ESP32S3_TOUCH_AMOLED_H */
