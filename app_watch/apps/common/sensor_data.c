/**
 * @file sensor_data.c
 * 传感器数据获取公共模块实现
 *
 * 单定时器架构: 一次读传感器，同时判断抬腕亮屏和摔倒检测
 * 抬腕: 静止记重力参考，抬腕时重力矢量大幅旋转(>阈值)即触发（佩戴朝向无关）
 * 摔倒: 陀螺仪瞬间剧烈变化 (静止g≈0-2, 摔倒g>300)，始终检测
 * 互斥: 一个触发后更新对方冷却时间，防止同时触发
 */

#include "sensor_data.h"
#include "watch_pages.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/stat.h>
#include <sensor/accel.h>
#include <sensor/gyro.h>
#include <nuttx/lcd/co5300.h>
#include <../../../../apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.h>
#include "../sos/sos.h"
#include "../launcher/dial.h"
#include <syslog.h>

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define SENSOR_LOG(fmt, ...) syslog(LOG_INFO, "[SENSOR] " fmt, ##__VA_ARGS__)

#define SENSOR_SAMPLE_INTERVAL   10000
#define SENSOR_POLL_TIMEOUT      1000

#define GRAVITY_1G               5.5f

#define WRIST_RAISE_COOLDOWN_MS  3000
#define GYRO_STILL_THRESH        10.0f   /* 静止判定：陀螺仪幅值 < 10 dps */
#define GRAVITY_ROTATE_DEV       0.8f    /* 重力矢量偏差 >0.8g ≈ 抬腕旋转 >47° */
#define FALL_GYRO_THRESH         500.0f
#define FALL_AZ_CHANGE_THRESH    0.8f
#define FALL_WINDOW_MS           800
#define FALL_COOLDOWN_MS         10000

#define STEP_DETECT_THRESHOLD    0.15f
#define STEP_MIN_INTERVAL_MS     200
#define STEP_MAX_INTERVAL_MS     3500
#define STEP_DEFAULT_LENGTH_M    0.65f
#define STEP_MIN_ACCEL_MAG       0.3f

#define SENSOR_MONITOR_PERIOD_MS 200

static int g_fd_accel = -1;
static int g_fd_gyro = -1;
static bool g_sensor_initialized = false;

static bool g_wrist_raise_enabled = true;
static lv_timer_t *g_sensor_monitor_timer = NULL;
static uint64_t g_wrist_raise_last_trigger = 0;
static uint64_t g_fall_last_trigger = 0;
static float g_last_az = -1.0f;
static float g_ref_ax = 0.0f;   /* 抬腕重力参考矢量（垂腕方向，佩戴朝向无关） */
static float g_ref_ay = 0.0f;
static float g_ref_az = 0.0f;
static bool  g_ref_valid = false;
static uint64_t g_fall_z_up_time = 0;

static int g_step_count = 0;
static float g_step_distance = 0.0f;
static bool g_step_paused = true;
static uint64_t g_step_last_peak_time = 0;
static float g_step_last_accel_mag = 0.0f;
static float g_step_accel_max = 0.0f;
static float g_step_accel_min = 100.0f;
static bool g_step_rising = true;
static int g_step_dir_change_samples = 0;
static float g_step_length = STEP_DEFAULT_LENGTH_M;

static uint64_t sensor_get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

int sensor_data_init(void)
{
    if (g_sensor_initialized) {
        return 0;
    }

    FAR const struct orb_metadata *meta;

    meta = ORB_ID(sensor_accel);
    g_fd_accel = orb_subscribe_multi(meta, 0);
    if (g_fd_accel < 0) {
        SENSOR_LOG("Failed to subscribe sensor_accel: %d", g_fd_accel);
        return -1;
    }
    orb_set_interval(g_fd_accel, SENSOR_SAMPLE_INTERVAL);

    meta = ORB_ID(sensor_gyro);
    g_fd_gyro = orb_subscribe_multi(meta, 0);
    if (g_fd_gyro < 0) {
        SENSOR_LOG("Failed to subscribe sensor_gyro: %d", g_fd_gyro);
        orb_unsubscribe(g_fd_accel);
        g_fd_accel = -1;
        return -1;
    }
    orb_set_interval(g_fd_gyro, SENSOR_SAMPLE_INTERVAL);

    g_sensor_initialized = true;
    g_last_az = -1.0f;
    g_ref_valid = false;
    SENSOR_LOG("Sensor data module initialized");
    return 0;
}

void sensor_data_deinit(void)
{
    if (g_fd_accel >= 0) {
        orb_unsubscribe(g_fd_accel);
        g_fd_accel = -1;
    }
    if (g_fd_gyro >= 0) {
        orb_unsubscribe(g_fd_gyro);
        g_fd_gyro = -1;
    }
    g_sensor_initialized = false;
}

int sensor_data_read(sensor_imu_data_t *data)
{
    if (!g_sensor_initialized || !data) {
        return -1;
    }

    struct pollfd fds[2];
    fds[0].fd = g_fd_accel;
    fds[0].events = POLLIN;
    fds[1].fd = g_fd_gyro;
    fds[1].events = POLLIN;

    int ret = poll(fds, 2, SENSOR_POLL_TIMEOUT);
    if (ret <= 0) {
        return -1;
    }

    FAR const struct orb_metadata *meta;

    if (fds[0].revents & POLLIN) {
        struct sensor_accel accel_data;
        meta = ORB_ID(sensor_accel);
        if (orb_copy(meta, g_fd_accel, &accel_data) == 0) {
            data->accel.x = accel_data.x;
            data->accel.y = accel_data.y;
            data->accel.z = accel_data.z;
            data->temperature = accel_data.temperature;
            data->timestamp = accel_data.timestamp;
        }
    }

    if (fds[1].revents & POLLIN) {
        struct sensor_gyro gyro_data;
        meta = ORB_ID(sensor_gyro);
        if (orb_copy(meta, g_fd_gyro, &gyro_data) == 0) {
            data->gyro.x = gyro_data.x;
            data->gyro.y = gyro_data.y;
            data->gyro.z = gyro_data.z;
            if (data->timestamp == 0) {
                data->timestamp = gyro_data.timestamp;
            }
        }
    }

    return 0;
}

/**
 * 统一传感器监控定时器
 * 一次读传感器, 同时判断抬腕亮屏(重力旋转)和摔倒(gyro>300)
 */
static void sensor_monitor_timer_cb(lv_timer_t *timer)
{
    sensor_imu_data_t data;
    if (sensor_data_read(&data) != 0) {
        return;
    }

    float gyro_mag = sqrtf(data.gyro.x * data.gyro.x +
                           data.gyro.y * data.gyro.y +
                           data.gyro.z * data.gyro.z);

    float ax_now = data.accel.x;
    float ay_now = data.accel.y;
    float az_now = data.accel.z;

    float az_prev = g_last_az;
    g_last_az = az_now;

    float accel_mag = sqrtf(data.accel.x * data.accel.x +
                            data.accel.y * data.accel.y +
                            data.accel.z * data.accel.z);
    (void)accel_mag;

    uint64_t now = sensor_get_time_ms();

    /* 抬腕亮屏：静止时（陀螺仪幅值小）记录重力参考矢量（佩戴朝向无关），
     * 抬腕时陀螺仪冲高→参考被冻结，当前重力与参考的偏差超过阈值即触发。
     * 这样既容忍慢速抬腕（中间过渡采样不漏），也适配任意佩戴朝向。 */
    if (gyro_mag < GYRO_STILL_THRESH) {
        g_ref_ax = ax_now;
        g_ref_ay = ay_now;
        g_ref_az = az_now;
        g_ref_valid = true;
    }

    float dax = ax_now - g_ref_ax;
    float day = ay_now - g_ref_ay;
    float daz = az_now - g_ref_az;
    float grav_dev = sqrtf(dax * dax + day * day + daz * daz);
    bool raised = (g_ref_valid && grav_dev > GRAVITY_ROTATE_DEV);

    bool gyro_high = (gyro_mag > FALL_GYRO_THRESH);
    float az_change = fabsf(az_now - az_prev);

    if (az_change > FALL_AZ_CHANGE_THRESH && g_step_paused) {
        g_fall_z_up_time = now;
    }

    bool in_fall_window = (g_fall_z_up_time > 0 && now - g_fall_z_up_time <= FALL_WINDOW_MS);
    bool fall_cond = (in_fall_window && gyro_high && g_step_paused);

    if (!g_step_paused) {
        sensor_step_counter_update(&data);
    }

    // printf("[SM] dev=%.2f raised=%d gyro=%.2f fall=%d fw=%d\n",
    //        grav_dev, raised, gyro_mag, fall_cond, in_fall_window);

    if (raised && g_wrist_raise_enabled && getchange() == 2) {
        if (now - g_wrist_raise_last_trigger > WRIST_RAISE_COOLDOWN_MS) {
            g_wrist_raise_last_trigger = now;
            g_fall_last_trigger = now;
            esp32s3_display_on();
            setchange(1);
            ft3168_display_timeout_setup();
            SENSOR_LOG("[WRIST] Screen on");
            g_ref_valid = false;  /* 触发后复位参考，待再次静止重建 */
        }
    }

    if (fall_cond) {
        if (now - g_fall_last_trigger > FALL_COOLDOWN_MS) {
            g_fall_last_trigger = now;
            g_wrist_raise_last_trigger = now;
            g_fall_z_up_time = 0;
        }
    }
}

void sensor_step_counter_reset(void)
{
    g_step_count = 0;
    g_step_distance = 0.0f;
    g_step_last_peak_time = 0;
    g_step_last_accel_mag = 0.0f;
    g_step_accel_max = 0.0f;
    g_step_accel_min = 100.0f;
    g_step_rising = true;
    g_step_dir_change_samples = 0;
    g_step_paused = false;
}

int sensor_step_counter_get_steps(void)
{
    return g_step_count;
}

float sensor_step_counter_get_distance(void)
{
    return g_step_distance;
}

void sensor_step_counter_update(const sensor_imu_data_t *data)
{
    if (!data || g_step_paused) {
        return;
    }

    float accel_mag = sqrtf(data->accel.x * data->accel.x +
                            data->accel.y * data->accel.y +
                            data->accel.z * data->accel.z);

    uint64_t now = sensor_get_time_ms();

    float adaptive_threshold = STEP_DETECT_THRESHOLD;
    if (accel_mag > GRAVITY_1G) {
        adaptive_threshold = 0.2f;
    } else if (accel_mag > 3.0f) {
        adaptive_threshold = 0.15f;
    } else if (accel_mag > 1.5f) {
        adaptive_threshold = 0.1f;
    } else {
        adaptive_threshold = 0.05f;
    }

    bool significant_change = fabsf(accel_mag - g_step_last_accel_mag) > 0.02f;

    if (accel_mag > g_step_last_accel_mag) {
        if (!g_step_rising) {
            g_step_rising = true;
            g_step_dir_change_samples = 0;
            float swing = g_step_accel_max - g_step_accel_min;
            if (swing > adaptive_threshold || (swing > 0.05f && significant_change)) {
                uint64_t step_interval = now - g_step_last_peak_time;
                if (g_step_last_peak_time != 0 && step_interval > STEP_MAX_INTERVAL_MS) {
                    g_step_last_peak_time = 0;
                    step_interval = 0;
                }
                if (g_step_last_peak_time == 0 ||
                    (step_interval >= STEP_MIN_INTERVAL_MS &&
                     step_interval <= STEP_MAX_INTERVAL_MS)) {
                    g_step_count++;
                    float adaptive_step_length = g_step_length;
                    if (accel_mag > 4.5f) {
                        adaptive_step_length = g_step_length * 1.2f;
                    } else if (accel_mag < 2.0f) {
                        adaptive_step_length = g_step_length * 0.8f;
                    }
                    g_step_distance += adaptive_step_length;
                    g_step_last_peak_time = now;
                    SENSOR_LOG("[STEP] #%d dist=%.2f swing=%.2f mag=%.2f thresh=%.2f",
                           g_step_count, g_step_distance, swing, accel_mag, adaptive_threshold);
                }
            }
            g_step_accel_max = accel_mag;
            g_step_accel_min = accel_mag;
        }
        g_step_dir_change_samples++;
        if (accel_mag > g_step_accel_max) {
            g_step_accel_max = accel_mag;
        }
    } else {
        if (g_step_rising) {
            g_step_rising = false;
            g_step_dir_change_samples = 0;
            g_step_accel_max = g_step_last_accel_mag;
            g_step_accel_min = accel_mag;
        }
        g_step_dir_change_samples++;
        if (accel_mag < g_step_accel_min) {
            g_step_accel_min = accel_mag;
        }
    }

    g_step_last_accel_mag = accel_mag;
}

void sensor_step_counter_pause(void)
{
    g_step_paused = true;
}

void sensor_step_counter_resume(void)
{
    g_step_paused = false;
}

bool sensor_step_counter_is_paused(void)
{
    return g_step_paused;
}

void sensor_step_set_length(float length_m)
{
    g_step_length = length_m;
}

bool wrist_raise_load_config(void)
{
    int fd = open(WRIST_RAISE_CONFIG_FILE, O_RDONLY);
    if (fd < 0) {
        g_wrist_raise_enabled = true;
        return false;
    }

    uint8_t val = 1;
    read(fd, &val, 1);
    close(fd);

    g_wrist_raise_enabled = (val != 0);
    SENSOR_LOG("[WRIST] Loaded config: enabled=%d", g_wrist_raise_enabled);
    return true;
}

bool wrist_raise_save_config(bool enabled)
{
    int fd = open(WRIST_RAISE_CONFIG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        SENSOR_LOG("[WRIST] Failed to save config");
        return false;
    }

    uint8_t val = enabled ? 1 : 0;
    write(fd, &val, 1);
    close(fd);

    g_wrist_raise_enabled = enabled;
    SENSOR_LOG("[WRIST] Saved config: enabled=%d", enabled);
    return true;
}

bool wrist_raise_get_enabled(void)
{
    return g_wrist_raise_enabled;
}

void wrist_raise_set_enabled(bool enabled)
{
    g_wrist_raise_enabled = enabled;
    wrist_raise_save_config(enabled);
}

void sensor_wrist_raise_timer_start(void)
{
    if (g_sensor_monitor_timer != NULL) {
        return;
    }

    if (sensor_data_init() != 0) {
        SENSOR_LOG("Failed to init sensor");
        return;
    }

    g_sensor_monitor_timer = lv_timer_create(sensor_monitor_timer_cb,
                                              SENSOR_MONITOR_PERIOD_MS, NULL);
    SENSOR_LOG("Monitor timer started");
}

void sensor_wrist_raise_timer_stop(void)
{
    if (g_sensor_monitor_timer != NULL) {
        lv_timer_del(g_sensor_monitor_timer);
        g_sensor_monitor_timer = NULL;
        SENSOR_LOG("Monitor timer stopped");
    }
}

bool sensor_wrist_raise_timer_is_running(void)
{
    return g_sensor_monitor_timer != NULL;
}

void sensor_fall_detect_timer_start(void)
{
    sensor_wrist_raise_timer_start();
}

void sensor_fall_detect_timer_stop(void)
{
    sensor_wrist_raise_timer_stop();
}
