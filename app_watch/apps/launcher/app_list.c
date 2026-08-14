#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

#include "app_list.h"
#include "dial.h"
#include "../../resource/resource.h"
#include "../common/watch_pages.h"
#include "../settings/settings.h"
#include "../calendar/calendar.h"
#include "../home_control/home_control.h"
#include "../stopwatch/stopwatch.h"
#include "../alarm/alarm.h"
#include "../sports/sports.h"
#include "../sos/sos.h"
#include "../tong_ai/tong_ai.h"
#include "launcher.h"
#include "../weather/weather.h"

/* 调试打印 — 统一使用 syslog 输出到 SD 卡 */
#define APP_LIST_LOG(fmt, ...) syslog(LOG_INFO, "[APP_LIST] " fmt, ##__VA_ARGS__)



#define MAX_APPS 12                     // 最多支持的应用数目(3页 x 4个)
#define APPS_PER_PAGE 4                 // 每页显示的应用数量
#define PAGE_COUNT 3                    // 总页数

/* 应用信息结构 */
typedef struct {
    const char *name;                  // 应用名称
    const lv_image_dsc_t *icon_src;    // 图标资源指针
    lv_obj_t *container;               // 容器对象
    lv_obj_t *icon;                    // 图标对象
    lv_obj_t *label;                   // 标签对象
    void (*click_cb)(lv_event_t *e);   // 点击回调
} app_info_t;

/* 全局变量 */
static app_info_t app_list[MAX_APPS];
static int app_count = 0;
static lv_obj_t *app_container = NULL;  // 应用列表容器
static lv_obj_t *pages[PAGE_COUNT];      // 页面对象数组
static int current_page = 0;             // 当前页面索引
static lv_obj_t *page_indicators[PAGE_COUNT];  // 页面指示灯数组

/* 样式 */
static lv_style_t style_black_bg;

/* 前向声明 */
static void switch_to_clock_display(lv_obj_t *cont);
static void handle_scroll_event(lv_obj_t *cont);
static void scroll_event_cb(lv_event_t *e);
static void app_icon_click_cb(lv_event_t *e);
static void update_page_indicators(int page_index);
static void create_page_indicators(lv_obj_t *parent)
    __attribute__((unused));

/**
 * @brief 应用图标点击回调
 */
static void app_icon_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t *obj = lv_event_get_target(e);
        
        /* 查找被点击的应用 */
        for (int i = 0; i < app_count; i++) {
            if (obj == app_list[i].container || obj == app_list[i].icon) {
                APP_LIST_LOG("App clicked: %s", app_list[i].name);
                
                /* 调用应用的点击回调 */
                if (app_list[i].click_cb) {
                    app_list[i].click_cb(e);
                }
                break;
            }
        }
    }
}

/**
 * @brief 更新页面指示灯状态
 */
static void update_page_indicators(int page_index)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == page_index) {
            /* 当前页面：白色，宽一些 */
            lv_obj_set_size(page_indicators[i], 12, 6);
            lv_obj_set_style_bg_color(page_indicators[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            /* 其他页面：灰色，正常大小 */
            lv_obj_set_size(page_indicators[i], 6, 6);
            lv_obj_set_style_bg_color(page_indicators[i], lv_color_hex(0x808080), 0);
        }
    }
}

/**
 * @brief 创建页面指示灯
 */
static void __attribute__((unused)) create_page_indicators(lv_obj_t *parent)
{
    /* 指示灯位于屏幕底部462px位置，使用相对对齐 */
    int indicator_width = 6;
    int indicator_height = 6;
    int indicator_spacing = 10;

    /* 创建一个容器来容纳所有指示灯，便于整体对齐 */
    lv_obj_t *indicator_container = lv_obj_create(parent);
    lv_obj_set_size(indicator_container, WATCH_SCREEN_WIDTH, 20);
    lv_obj_set_style_bg_opa(indicator_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(indicator_container, 0, 0);
    /* 将容器对齐到屏幕底部，距离底部40px */
    lv_obj_align(indicator_container, LV_ALIGN_BOTTOM_MID, 0, -40);
    
    /* 计算起始位置，使指示灯水平居中 */
    int total_width = PAGE_COUNT * indicator_width + (PAGE_COUNT - 1) * indicator_spacing;
    int start_x = (WATCH_SCREEN_WIDTH - total_width) / 2;
    
    for (int i = 0; i < PAGE_COUNT; i++) {
        page_indicators[i] = lv_obj_create(indicator_container);
        lv_obj_set_size(page_indicators[i], indicator_width, indicator_height);
        lv_obj_set_style_bg_color(page_indicators[i], lv_color_hex(0x808080), 0);
        lv_obj_set_style_bg_opa(page_indicators[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(page_indicators[i], 0, 0);
        lv_obj_set_style_radius(page_indicators[i], 3, 0);
        /* 设置相对位置 */
        lv_obj_set_pos(page_indicators[i], start_x + i * (indicator_width + indicator_spacing), 7);
        lv_obj_clear_flag(page_indicators[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(page_indicators[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    
    /* 初始化指示灯状态 */
    update_page_indicators(0);
}

/**
 * @brief 切换到表盘显示
 */
static void switch_to_clock_display(lv_obj_t *cont)
{
    APP_LIST_LOG("Right swipe: returning to dial");
    
    /* 先显示隐藏的表盘 */
    dial_show();
    
    /* 从页面栈弹出 */
    lv_watch_pop_page(cont);
    
    /* 异步删除应用列表，避免事件回调死锁 */
    lv_obj_delete_async(cont);
    app_container = NULL;
}

/**
 * @brief 处理滚动事件
 */
static void handle_scroll_event(lv_obj_t *cont)
{
    lv_coord_t x = lv_obj_get_scroll_x(cont);
    // lv_coord_t w = lv_obj_get_content_width(cont);
    // lv_coord_t w_visible = lv_obj_get_width(cont);
    
    // APP_LIST_LOG("scroll x: %d, w: %d, w_visible: %d", x, w, w_visible);

    /* 计算当前页面索引 */
    int page_width = WATCH_SCREEN_WIDTH;
    int new_page = current_page;
    
    // 基于当前页面的滚动偏移量判断
    lv_coord_t offset = x - current_page * page_width;
    
    if (offset > page_width / 2) {
        // 向右滑动超过半页，切换到下一页
        new_page = current_page + 1;
    } else if (offset < -page_width / 2) {
        // 向左滑动超过半页，切换到上一页
        new_page = current_page - 1;
    }
    
    // 边界限制
    if (new_page < 0) new_page = 0;
    if (new_page >= PAGE_COUNT) new_page = PAGE_COUNT - 1;
    
    /* 如果页面发生变化，更新指示灯 */
    // if (new_page != current_page) {
    //     current_page = new_page;
    //     update_page_indicators(current_page);
    // }

    /* 如果向右滑动超过了左边界，切换到时钟界面 */
    if (x < -50) {
        switch_to_clock_display(cont);
    }
}

/**
 * @brief 滚动事件回调
 */
static void scroll_event_cb(lv_event_t *e)
{
    lv_obj_t *cont = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_SCROLL) {
        handle_scroll_event(cont);
    }
    /* 移除 SCROLL_END 处理，让 LVGL 的自然吸附生效，避免死锁 */
}

/**
 * @brief 创建单个应用图标项
 */
static void create_app_item(lv_obj_t *parent, int index, int page_index)
{
    app_info_t *app = &app_list[index];

    /* 计算位置 - 2x2布局 */
    int pos_in_page = index - page_index * APPS_PER_PAGE;
    int col = pos_in_page % 2;
    int row = pos_in_page / 2;

    /* 计算坐标 - 130x130图标 */
    /* 左上方第一个图标坐标(52, 51) */
    /* 第二个图标坐标(228, 51)，两个图标间距46 */
    /* 左边上方应用图标和下方图标相距79px */
    /* 下方两图标相距也是46px */
    lv_coord_t icon_x = (col == 0) ? 52 : 228;
    lv_coord_t icon_y = (row == 0) ? 51 : (51 + 130 + 79);  /* 上方51，下方 = 51 + 130 + 79 = 260 */

    /* 创建应用容器 */
    app->container = lv_obj_create(parent);
    lv_obj_set_size(app->container, 130, 130);
    lv_obj_set_pos(app->container, icon_x, icon_y);
    lv_obj_clear_flag(app->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(app->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(app->container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(app->container, 0, 0);

    /* 创建图标 */
    app->icon = lv_img_create(app->container);
    lv_img_set_src(app->icon, app->icon_src);
    // lv_obj_set_width(app->icon, LV_SIZE_CONTENT);
    // lv_obj_set_height(app->icon, LV_SIZE_CONTENT);
    lv_obj_align(app->icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(app->icon, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(app->icon, LV_OBJ_FLAG_SCROLLABLE);

    /* 创建标签 - 在图标下方 */
    app->label = lv_label_create(parent);
    lv_label_set_text(app->label, app->name);
    lv_obj_set_style_text_font(app->label, vw_resource_get_font(WATCH_REGULAR_FONT "_24"), 0);
    lv_obj_set_style_text_color(app->label, lv_color_hex(0xFFFFFF), 0);
    // lv_obj_set_style_text_font(app->label, &activity_24px_chinese_font, 0);
    lv_obj_set_style_text_align(app->label, LV_TEXT_ALIGN_CENTER, 0);

    /* 标签位置 - 在图标正下方居中，距离图标6px */
    lv_obj_set_pos(app->label, icon_x, icon_y + 130 + 6);  /* 图标下方6px */
    lv_obj_set_width(app->label, 130);  /* 设置宽度与图标相同以便居中 */

    /* 添加点击事件 */
    lv_obj_add_event_cb(app->container, app_icon_click_cb, LV_EVENT_CLICKED, NULL);
}

/**
 * @brief 创建应用列表页面
 */
lv_obj_t *app_tile_setup(lv_obj_t *parent)
{
    app_count = 0;
    current_page = 0;
    
    /* 初始化样式 */
    lv_style_init(&style_black_bg);
    lv_style_set_bg_color(&style_black_bg, lv_color_black());
    lv_style_set_border_width(&style_black_bg, 0);
    lv_style_set_pad_all(&style_black_bg, 0);
    lv_style_set_pad_gap(&style_black_bg, 0);

    /* 创建应用列表容器 */
    app_container = lv_obj_create(parent);
    lv_obj_set_size(app_container, WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
    lv_obj_set_pos(app_container, 0, 0);  // 明确设置位置
    lv_obj_center(app_container);  /* 居中显示 */
    lv_obj_set_layout(app_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(app_container, LV_FLEX_FLOW_ROW);  // 水平排列页面
    lv_obj_set_style_pad_all(app_container, 0, 0);
    lv_obj_set_style_border_width(app_container, 0, 0);
    lv_obj_add_style(app_container, &style_black_bg, 0);
    
    /* 设置滚动方向为水平方向（左右滑动） */
    lv_obj_set_scroll_dir(app_container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(app_container, LV_SCROLL_SNAP_START);  /* 启用吸附效果 */
    lv_obj_set_scrollbar_mode(app_container, LV_SCROLLBAR_MODE_OFF);
    
    /* 添加滚动事件处理 */
    lv_obj_add_event_cb(app_container, scroll_event_cb, LV_EVENT_SCROLL, NULL);

    /* 创建两页 */
    for (int page = 0; page < PAGE_COUNT; page++) {
        pages[page] = lv_obj_create(app_container);
        lv_obj_set_size(pages[page], WATCH_SCREEN_WIDTH, WATCH_SCREEN_HEIGHT);
        lv_obj_set_style_bg_color(pages[page], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(pages[page], 255, 0);
        lv_obj_set_style_border_width(pages[page], 0, 0);  // 确保无边框
        lv_obj_set_style_pad_all(pages[page], 0, 0);       // 确保无内边距
        lv_obj_clear_flag(pages[page], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 注册应用 */
    /* 第1页应用 */
    register_app("SOS求助", vw_resource_get_img("icon_app_sos"), sos_app_click_callback);
    register_app("智能家居", vw_resource_get_img("icon_app_home"), home_control_app_click_callback);
    register_app("设置", vw_resource_get_img("icon_app_set"), settings_app_click_callback);

    /* 第2页应用 */
    register_app("运动", vw_resource_get_img("icon_app_sport"), sport_app_click_callback);
    register_app("闹钟", vw_resource_get_img("icon_app_alarm"), alarm_app_click_callback);
    register_app("天气", vw_resource_get_img("icon_app_weather"), weather_app_click_callback);
    register_app("秒表", vw_resource_get_img("icon_app_stopwatch"), stopwatch_app_click_callback);

    /* 第3页应用 */
    register_app("日历", vw_resource_get_img("icon_app_calendar"), calendar_app_click_callback);
    register_app("小通AI", vw_resource_get_img("icon_app_ai"), tong_ai_app_click_callback);
    /* 创建应用图标 */
    for (int i = 0; i < app_count; i++) {
        int page_index = i / APPS_PER_PAGE;
        if (page_index < PAGE_COUNT) {
            create_app_item(pages[page_index], i, page_index);
        }
    }

    /* 创建页面指示灯 - 创建在app_container上，这样删除容器时指示灯也会被删除 */
    // create_page_indicators(parent);

    return app_container;
}

/**
 * @brief 注册应用
 */
void register_app(const char *name, const lv_image_dsc_t *icon_src, void (*click_cb)(lv_event_t *e))
{
    if (app_count >= MAX_APPS) {
        APP_LIST_LOG("Max apps reached!");

        return;
    }

    app_list[app_count].name = name;
    app_list[app_count].icon_src = icon_src;
    app_list[app_count].click_cb = click_cb;
    app_list[app_count].container = NULL;
    app_list[app_count].icon = NULL;
    app_list[app_count].label = NULL;

    app_count++;
}
