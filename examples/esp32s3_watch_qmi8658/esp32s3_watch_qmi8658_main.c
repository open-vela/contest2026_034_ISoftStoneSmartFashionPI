/****************************************************************************
 * apps/examples/esp32s3_watch_qmi8658/esp32s3_watch_qmi8658_main.c
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
#include <poll.h>
#include <errno.h>
#include <inttypes.h>
#include <sensor/accel.h>
#include <sensor/gyro.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define READ_TIMES    100   /* Default number of sensor reads */
#define POLL_TIMEOUT  5000  /* Poll timeout in milliseconds */
#define SAMPLE_INTERVAL 10000 /* Sample interval in microseconds (10ms = 100Hz) */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void print_accel_data(FAR const struct sensor_accel *data)
{
  printf("Accel: timestamp:%" PRIu64 " "
         "x:%.2f y:%.2f z:%.2f m/s^2 "
         "temp:%.2f C\n",
         data->timestamp,
         data->x, data->y, data->z,
         data->temperature);
}

static void print_gyro_data(FAR const struct sensor_gyro *data)
{
  printf("Gyro:  timestamp:%" PRIu64 " "
         "x:%.2f y:%.2f z:%.2f rad/s "
         "temp:%.2f C\n",
         data->timestamp,
         data->x, data->y, data->z,
         data->temperature);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const struct orb_metadata *meta_accel;
  FAR const struct orb_metadata *meta_gyro;
  struct pollfd fds[2];
  struct sensor_accel   esp32s3_watch_accel_data;   // 用于存储加速度计数据
  struct sensor_gyro    esp32s3_watch_gyro_data;    // 用于存储陀螺仪数据
  int ret = OK;
  int fd_accel;
  int fd_gyro;
  int i;

  /* Subscribe to accelerometer sensor */

  printf("Subscribing to accelerometer sensor...\n");
  meta_accel = ORB_ID(sensor_accel);
  fd_accel = orb_subscribe_multi(meta_accel, 0);
  if (fd_accel < 0)
    {
      printf("Failed to subscribe sensor_accel, ret: %d\n", fd_accel);
      return fd_accel;
    }
  printf("Accelerometer subscribed successfully, fd=%d\n", fd_accel);

  /* Set accelerometer update interval to 10ms (100Hz) */

  ret = orb_set_interval(fd_accel, SAMPLE_INTERVAL);
  printf("Set accelerometer interval to %d us, ret=%d\n", SAMPLE_INTERVAL, ret);

  /* Subscribe to gyroscope sensor */

  printf("Subscribing to gyroscope sensor...\n");
  meta_gyro = ORB_ID(sensor_gyro);
  fd_gyro = orb_subscribe_multi(meta_gyro, 0);
  if (fd_gyro < 0)
    {
      printf("Failed to subscribe sensor_gyro, ret: %d\n", fd_gyro);
      orb_unsubscribe(fd_accel);
      return fd_gyro;
    }
  printf("Gyroscope subscribed successfully, fd=%d\n", fd_gyro);

  /* Set gyroscope update interval to 10ms (100Hz) */

  ret = orb_set_interval(fd_gyro, SAMPLE_INTERVAL);
  printf("Set gyroscope interval to %d us, ret=%d\n", SAMPLE_INTERVAL, ret);

  /* Setup poll structure for both sensors */

  printf("Setting up poll for sensor data...\n");
  fds[0].fd = fd_accel;
  fds[0].events = POLLIN;
  fds[1].fd = fd_gyro;
  fds[1].events = POLLIN;

  printf("Starting sensor data collection (%d reads)...\n", READ_TIMES);

  /* Test loop: read and output sensor data */

  for (i = 0; i < READ_TIMES; i++)
    {
      if (poll(fds, 2, POLL_TIMEOUT) > 0)
        {
          /* Read and output accelerometer data */

          if (fds[0].revents & POLLIN)
            {
              ret = orb_copy(meta_accel, fd_accel, &esp32s3_watch_accel_data);
              if (ret == OK)
                {
                  print_accel_data(&esp32s3_watch_accel_data);
                }
            }

          /* Read and output gyroscope data */

          if (fds[1].revents & POLLIN)
            {
              ret = orb_copy(meta_gyro, fd_gyro, &esp32s3_watch_gyro_data);
              if (ret == OK)
                {
                  print_gyro_data(&esp32s3_watch_gyro_data);
                }
            }
        }
      else if (errno != EINTR)
        {
          printf("Poll timeout after %d milliseconds, errno: %d\n",
                 POLL_TIMEOUT, errno);
          break;
        }
    }

  /* Cleanup: unsubscribe all sensors */

  orb_unsubscribe(fd_accel);
  orb_unsubscribe(fd_gyro);

  printf("QMI8658 sensor test completed.\n");

  return ret;
}
