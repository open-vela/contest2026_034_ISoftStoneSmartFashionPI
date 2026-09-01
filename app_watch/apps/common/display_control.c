/**
 * @file display_control.c
 * 定时息屏控制实现。
 *
 * 通过 LVGL 定时器监控显示不活动时间，超时后调用 esp32s3_display_off()
 * 关闭屏幕；检测到触摸活动后调用 esp32s3_display_on() 重新点亮。
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <nuttx/lcd/co5300.h>
#include <lvgl/lvgl.h>

#define TIMEOUT_FILE    "/mnt/spif/display_timeout"
#define DEFAULT_TIMEOUT  10
#define TIMER_PERIOD_MS  500

static int       g_timeout_sec  = DEFAULT_TIMEOUT;
static int       g_change       = 0;    /* 0=正常, 1=亮屏中, 2=需亮屏 */
static bool      g_screen_off   = false;
static lv_timer_t *g_timer      = NULL;
/* 自动熄屏开关：语音交互场景（表情潮玩模式、小通AI页面）调用
 * display_timeout_disable() 挂起熄屏，退出时 display_timeout_enable()
 * 恢复。禁用期间定时器保持运行但不执行熄屏。 */
static bool      g_timeout_disabled = false;

static void timeout_timer_cb(lv_timer_t *timer)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return;

    if (g_timeout_disabled) return;

    uint32_t inactive_ms = lv_disp_get_inactive_time(disp);

    if (g_screen_off) {
        if (inactive_ms < 300) {
            esp32s3_display_on();
            g_screen_off = false;
            g_change = 0;
            syslog(LOG_INFO, "[DISPLAY] touch detected, screen on\n");
        }
    } else {
        uint32_t threshold = (uint32_t)g_timeout_sec * 1000;
        if (inactive_ms >= threshold) {
            esp32s3_display_off();
            g_screen_off = true;
            g_change = 2;
            syslog(LOG_INFO, "[DISPLAY] timeout %ds, screen off\n",
                   g_timeout_sec);
        }
    }
}

void display_set_timeout(int timeout)
{
    if (timeout < 5 || timeout > 300) timeout = DEFAULT_TIMEOUT;
    g_timeout_sec = timeout;

    int fd = open(TIMEOUT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d\n", timeout);
        write(fd, buf, len);
        close(fd);
    }

    if (!g_timer) {
        g_timer = lv_timer_create(timeout_timer_cb, TIMER_PERIOD_MS, NULL);
    }
    syslog(LOG_INFO, "[DISPLAY] set timeout = %d s\n", timeout);
}

int display_get_timeout(void)
{
    int fd = open(TIMEOUT_FILE, O_RDONLY);
    if (fd >= 0) {
        char buf[16] = {0};
        read(fd, buf, sizeof(buf) - 1);
        close(fd);
        int t = atoi(buf);
        if (t >= 5 && t <= 300) g_timeout_sec = t;
    }
    return g_timeout_sec;
}

void ft3168_display_timeout_setup(void)
{
    if (!g_timer) {
        g_timer = lv_timer_create(timeout_timer_cb, TIMER_PERIOD_MS, NULL);
    }
    if (g_screen_off) {
        esp32s3_display_on();
        g_screen_off = false;
        g_change = 0;
    }
}

void setNull_display_timeout_timer(void)
{
    if (g_timer) {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
}

/**
 * @brief 暂停自动熄屏（语音交互场景：表情潮玩模式、小通AI页面）。
 *
 * 只挂起熄屏判定，定时器不删除；若屏幕已熄则立即点亮。
 * 与语音侧 180s 空闲待机（表情隐藏）相互独立，互不影响。
 */
void display_timeout_disable(void)
{
    if (!g_timeout_disabled) {
        g_timeout_disabled = true;
        if (g_screen_off) {
            esp32s3_display_on();
            g_screen_off = false;
            g_change = 0;
        }
        syslog(LOG_INFO, "[DISPLAY] auto off disabled\n");
    }
}

/**
 * @brief 恢复自动熄屏，并从当前时刻重新计时（避免退出语音场景后
 * 按上一次触摸时间立即熄屏）。
 */
void display_timeout_enable(void)
{
    if (!g_timeout_disabled) return;
    g_timeout_disabled = false;

    lv_disp_t *disp = lv_disp_get_default();
    if (disp) lv_display_trigger_activity(disp);

    syslog(LOG_INFO, "[DISPLAY] auto off enabled\n");
}

int getchange(void)
{
    return g_change;
}

void setchange(int v)
{
    g_change = v;
}