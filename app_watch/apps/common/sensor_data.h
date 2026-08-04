/**
 * @file sensor_data.h
 * 传感器数据获取公共模块 - 陀螺仪/加速度计应用层接口
 */

#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#include <stdbool.h>
#include <lvgl/lvgl.h>

#define WRIST_RAISE_CONFIG_FILE "/mnt/sd/wrist_raise.dat"

#define STEP_LENGTH_WALKING_M        0.60f
#define STEP_LENGTH_RUNNING_M        0.80f

typedef struct {
    float x;
    float y;
    float z;
} sensor_vec3_t;

typedef struct {
    sensor_vec3_t accel;
    sensor_vec3_t gyro;
    float temperature;
    uint64_t timestamp;
} sensor_imu_data_t;

int sensor_data_init(void);
void sensor_data_deinit(void);
int sensor_data_read(sensor_imu_data_t *data);

bool sensor_detect_wrist_raise(void);
bool sensor_detect_fall(void);

void sensor_step_counter_reset(void);
int sensor_step_counter_get_steps(void);
float sensor_step_counter_get_distance(void);
void sensor_step_counter_update(const sensor_imu_data_t *data);
void sensor_step_counter_pause(void);
void sensor_step_counter_resume(void);
bool sensor_step_counter_is_paused(void);
void sensor_step_set_length(float length_m);

bool wrist_raise_load_config(void);
bool wrist_raise_save_config(bool enabled);
bool wrist_raise_get_enabled(void);
void wrist_raise_set_enabled(bool enabled);

void sensor_wrist_raise_timer_start(void);
void sensor_wrist_raise_timer_stop(void);
bool sensor_wrist_raise_timer_is_running(void);

void sensor_fall_detect_timer_start(void);
void sensor_fall_detect_timer_stop(void);

#endif /* SENSOR_DATA_H */
