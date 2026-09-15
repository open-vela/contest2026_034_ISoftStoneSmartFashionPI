#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <nuttx/timers/rtc.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TEST_DURATION_SECONDS  30

#define LOG_INFO(fmt, ...)    printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_SUCCESS(fmt, ...) printf("[SUCCESS] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   printf("[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_TEST(fmt, ...)    printf("[TEST] " fmt "\n", ##__VA_ARGS__)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void print_datetime(FAR const struct tm *datetime)
{
  printf("%04d-%02d-%02d %02d:%02d:%02d (Weekday: %d)\n",
         datetime->tm_year + 1900,
         datetime->tm_mon + 1,
         datetime->tm_mday,
         datetime->tm_hour,
         datetime->tm_min,
         datetime->tm_sec,
         datetime->tm_wday);
}

static int test_rtc_read(int fd)
{
  struct rtc_time rtctime;
  int ret;

  LOG_TEST("Testing RTC read functionality...");

  ret = ioctl(fd, RTC_RD_TIME, (unsigned long)&rtctime);
  if (ret < 0)
    {
      LOG_ERROR("Failed to read RTC time: %d", errno);
      return ERROR;
    }

  LOG_INFO("Current RTC time:");
  print_datetime((FAR const struct tm *)&rtctime);
  LOG_SUCCESS("RTC read test PASSED");

  return OK;
}

static int test_rtc_set(int fd)
{
  struct rtc_time rtctime;
  struct rtc_time readback;
  int ret;

  LOG_TEST("Testing RTC set functionality...");

  memset(&rtctime, 0, sizeof(rtctime));
  rtctime.tm_sec   = 00;
  rtctime.tm_min   = 22;
  rtctime.tm_hour  = 14;
  rtctime.tm_mday  = 19;
  rtctime.tm_mon   = 4;
  rtctime.tm_year  = 126;
  rtctime.tm_wday  = 1;

  LOG_INFO("Setting RTC time to 2026-05-19 14:22:00...");
  print_datetime((FAR const struct tm *)&rtctime);

  ret = ioctl(fd, RTC_SET_TIME, (unsigned long)&rtctime);
  if (ret < 0)
    {
      LOG_ERROR("Failed to set RTC time: %d", errno);
      return ERROR;
    }

  ret = ioctl(fd, RTC_RD_TIME, (unsigned long)&readback);
  if (ret < 0)
    {
      LOG_ERROR("Failed to read back RTC time: %d", errno);
      return ERROR;
    }

  LOG_INFO("Time after write:");
  print_datetime((FAR const struct tm *)&readback);

  if (memcmp(&rtctime, &readback, sizeof(struct rtc_time)) == 0)
    {
      LOG_SUCCESS("RTC set test PASSED");
      return OK;
    }
  else
    {
      LOG_ERROR("RTC set test FAILED - time mismatch");
      return ERROR;
    }
}

static int test_rtc_continuous_read(int fd)
{
  struct rtc_time rtctime;
  int i;
  int ret;

  LOG_TEST("Testing continuous RTC time reading (%d seconds)...", TEST_DURATION_SECONDS);

  for (i = 0; i < TEST_DURATION_SECONDS; i++)
    {
      ret = ioctl(fd, RTC_RD_TIME, (unsigned long)&rtctime);
      if (ret < 0)
        {
          LOG_ERROR("Failed to read RTC time at iteration %d: %d", i, errno);
          return ERROR;
        }

      printf("\rCurrent RTC time: ");
      print_datetime((FAR const struct tm *)&rtctime);
      fflush(stdout);

      sleep(1);
    }

  printf("\n");
  LOG_SUCCESS("Continuous read test PASSED");
  return OK;
}

static int test_rtc_time_increment(int fd)
{
  struct rtc_time rtctime1;
  struct rtc_time rtctime2;
  int ret;

  LOG_TEST("Testing RTC time increment...");

  ret = ioctl(fd, RTC_RD_TIME, (unsigned long)&rtctime1);
  if (ret < 0)
    {
      LOG_ERROR("Failed to read RTC time: %d", errno);
      return ERROR;
    }

  LOG_INFO("Initial time:");
  print_datetime((FAR const struct tm *)&rtctime1);

  sleep(5);

  ret = ioctl(fd, RTC_RD_TIME, (unsigned long)&rtctime2);
  if (ret < 0)
    {
      LOG_ERROR("Failed to read RTC time: %d", errno);
      return ERROR;
    }

  LOG_INFO("Time after 5 seconds:");
  print_datetime((FAR const struct tm *)&rtctime2);

  if (rtctime2.tm_sec >= rtctime1.tm_sec + 5 ||
      (rtctime2.tm_sec + 60 - rtctime1.tm_sec) % 60 >= 5)
    {
      LOG_SUCCESS("RTC time increment test PASSED");
      return OK;
    }
  else
    {
      LOG_ERROR("RTC time increment test FAILED - time did not increment properly");
      return ERROR;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int fd;
  int test_results = 0;

  LOG_INFO("=============================================");
  LOG_INFO("   ESP32-S3 PCF85063 RTC Test Application");
  LOG_INFO("=============================================");
  LOG_INFO("Test duration: %d seconds", TEST_DURATION_SECONDS);
  LOG_INFO("=============================================");

  fd = open("/dev/rtc0", O_RDWR);
  if (fd < 0)
    {
      LOG_ERROR("Failed to open RTC device: %d", errno);
      LOG_ERROR("Please check:");
      LOG_ERROR("  1. PCF85063 is properly connected");
      LOG_ERROR("  2. I2C bus is configured correctly");
      LOG_ERROR("  3. PCF85063 driver is enabled in kernel config");
      return EXIT_FAILURE;
    }

  LOG_INFO("RTC device opened successfully");

  if (test_rtc_read(fd) == OK)
    {
      test_results++;
    }

  if (test_rtc_set(fd) == OK)
    {
      test_results++;
    }

  if (test_rtc_time_increment(fd) == OK)
    {
      test_results++;
    }

  if (test_rtc_continuous_read(fd) == OK)
    {
      test_results++;
    }

  close(fd);

  LOG_INFO("=============================================");
  LOG_INFO("              Test Results Summary");
  LOG_INFO("=============================================");
  LOG_INFO("Total tests: 4");
  LOG_INFO("Passed: %d", test_results);
  LOG_INFO("Failed: %d", 4 - test_results);

  if (test_results == 4)
    {
      LOG_SUCCESS("ALL TESTS PASSED!");
      LOG_SUCCESS("PCF85063 RTC is working correctly");
      return EXIT_SUCCESS;
    }
  else
    {
      LOG_ERROR("Some tests FAILED");
      LOG_ERROR("Please check your PCF85063 connection");
      return EXIT_FAILURE;
    }
}