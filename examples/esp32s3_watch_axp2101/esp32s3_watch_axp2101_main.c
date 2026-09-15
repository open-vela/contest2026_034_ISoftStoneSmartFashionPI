/****************************************************************************
 * apps/examples/esp32s3_watch_axp2101/esp32s3_watch_axp2101_main.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <nuttx/power/battery_ioctl.h>
#include <nuttx/power/axp2101.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  int ret;
  int soc;
  int ret_on;
  int ret_off;
  int ret_pmu_status;
  int ret_pmu_charge_status;

  /* Open the battery device */
  fd = open("/dev/bat0", O_RDONLY);
  if (fd < 0)
    {
      printf("ERROR: Failed to open /dev/bat0: %d\n", errno);
      return ERROR;
    }

  printf("Battery device opened successfully\n");

  /* Read battery state of charge (SOC) */
  ret = ioctl(fd, BATIOC_OPERATE, AXP2101_BATTERY_IOCTL_GET_SOC);
  if (ret < 0)
    {
      printf("ERROR: Failed to read battery SOC: %d\n", errno);
      close(fd);
      return ERROR;
    }

  /* The SOC value is returned as the ioctl return value */
  soc = ret;
  printf("Battery SOC: %d%%\n", soc);

  
  /* Read battery state of charge (SOC) again */
  int soc_2;
  soc_2 = axp2101_get_pmu_soc();
  printf("Battery SOC again %d%%: \n", soc_2);


  /* 获取电池poweron状态的原因 */
  ret_on = ioctl(fd, BATIOC_OPERATE, AXP2101_BATTERY_IOCTL_GET_POWERON_STATUS);
  if (ret_on < 0)
    {
      printf("Read battery poweron resaon ERROR: %d\n", errno);
      close(fd);
      return ERROR;
    }
  printf("Read battery poweron resaon: %d\n", ret_on);


  /* 获取电池poweroff状态的原因 */
  ret_off = ioctl(fd, BATIOC_OPERATE, AXP2101_BATTERY_IOCTL_GET_POWEROFF_STATUS);
  if (ret_off < 0)
    {
      printf("Read battery poweroff reason ERROR: %d\n", errno);
      close(fd);
      return ERROR;
    }
  printf("Read battery poweroff reason: %d\n", ret_off);


    /* 获取电池的状态 */
  ret_pmu_status = ioctl(fd, BATIOC_OPERATE, AXP2101_BATTERY_IOCTL_GET_PMU_STATUS);
  if (ret_pmu_status < 0)
    {
      printf("Read battery pmu status errorno: %d\n", errno);
      close(fd);
      return ERROR;
    }
  printf("Read battery pmu status: %d\n", ret_pmu_status);


    /* 获取电池充电状态 */
  ret_pmu_charge_status = ioctl(fd, BATIOC_OPERATE, AXP2101_BATTERY_IOCTL_GET_PMU_CHARGE_STATUS);
  if (ret_pmu_charge_status < 0)
    {
      printf("Read battery pmu charge status errorno: %d\n", errno);
      close(fd);
      return ERROR;
    }
  printf("Read battery pmu charge status: %#X\n", ret_pmu_charge_status);

  /* Read battery charge status */
  int charge_status_2=0;
  charge_status_2 =  axp2101_get_pmu_charge_status();
  printf("Read battery pmu charge status again: %#X \n", charge_status_2);

  int dcdc1 = 0;
  dcdc1 = axp2101_get_dcdc1_voltage_ext();
  printf("Read battery dcdc1: %d mv\n", dcdc1);

  int dcdc2 = 0;
  dcdc2 = axp2101_get_dcdc2_voltage_ext();
  printf("Read battery dcdc2: %d mv\n", dcdc2);


  int dcdc3 = 0;
  dcdc3 = axp2101_get_dcdc3_voltage_ext();
  printf("Read battery dcdc3: %d mv\n", dcdc3);


  int dcdc4 = 0;
  dcdc4 = axp2101_get_dcdc4_voltage_ext();
  printf("Read battery dcdc4: %d mv\n", dcdc4);

  int aldo1 = 0;
  aldo1 = axp2101_get_aldo1_voltage_ext();
  printf("Read battery aldo1: %d mv\n", aldo1);

  int aldo2 = 0;
  aldo2 = axp2101_get_aldo2_voltage_ext();
  printf("Read battery aldo2: %d mv\n", aldo2);

  int aldo3 = 0;
  aldo3 = axp2101_get_aldo3_voltage_ext();
  printf("Read battery aldo3: %d mv\n", aldo3);

  int aldo4 = 0;
  aldo4 = axp2101_get_aldo4_voltage_ext();
  printf("Read battery aldo4: %d mv\n", aldo4);

  int bldo1 = 0;
  bldo1 = axp2101_get_bldo1_voltage_ext();
  printf("Read battery bldo1: %d mv\n", bldo1);

  int bldo2 = 0;
  bldo2 = axp2101_get_bldo2_voltage_ext();
  printf("Read battery bldo2: %d mv\n", bldo2);

  int cpusldo = 0;
  cpusldo = axp2101_get_cpusldo_voltage_ext();
  printf("Read battery cpusldo: %d mv\n", cpusldo);

  int irq_status_1 = 0;
  irq_status_1 = axp2101_get_irq_status_1_ext();
  printf("Read battery irq_status_1: %d\n", irq_status_1);
  printf("  二进制是：");
  for (int i = 7; i >= 0; i--)
  {
      printf("%d", (irq_status_1 >> i) & 1);
  }
  printf("\n");



  /* Close the device */
  close(fd);

  return OK;
}
