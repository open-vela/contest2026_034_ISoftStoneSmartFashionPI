/**
 * @file home_control.c
 * 智能家居控制模块实现
 *
 * 通过 WiFi 与本地控制服务器通信，提供 12 路设备的开关控制能力。
 * 通信流程：
 *   1. UDP 广播发现局域网内控制服务器（端口 9999）
 *   2. TCP 短连接向服务器发送数字命令
 *
 * 命令编号规则：
 *   普通设备：cmd_on = index * 2 + 1, cmd_off = index * 2 + 2
 *   窗帘（带子开关）：cmd_on = 23（上电）, cmd_off = 24（断电）
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netutils/netlib.h>

#include "home_control.h"
#include "../settings/settings_wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_CONTEST2026_WATCH_DEBUG
#define HC_LOG(fmt, ...)  printf("[HomeControl] " fmt "\n", ##__VA_ARGS__)
#else
#define HC_LOG(fmt, ...)
#endif

#define HC_DISCOVERY_PORT     9999
#define HC_DISCOVERY_MAGIC    "DISCOVER_REQ"
#define HC_RESPONSE_MAGIC     "DISCOVER_RESP"
#define HC_DISCOVERY_TIMEOUT  5

#define HC_DEVICE_COUNT 12

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 智能家居设备信息 */
typedef struct {
    const char *name;           /* 设备名称 */
    bool        status;         /* 当前开关状态 */
    bool        has_sub_switch; /* 是否带子开关（窗帘） */
} hc_device_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static hc_device_t s_hc_devices[HC_DEVICE_COUNT];
static bool s_hc_devices_initialized = false;

/* 服务器信息 */
static char s_hc_server_ip[64]     = {0};
static int  s_hc_server_port        = 0;
static bool s_hc_server_discovered  = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * @brief 初始化设备列表
 */
static void hc_init_devices(void)
{
  if (s_hc_devices_initialized)
    {
      return;
    }

  s_hc_devices[0]  = (hc_device_t){"电视",       false, false};
  s_hc_devices[1]  = (hc_device_t){"空调",       false, false};
  s_hc_devices[2]  = (hc_device_t){"地暖",       false, false};
  s_hc_devices[3]  = (hc_device_t){"新风",       false, false};
  s_hc_devices[4]  = (hc_device_t){"客厅氛围灯", false, false};
  s_hc_devices[5]  = (hc_device_t){"客厅灯",     false, false};
  s_hc_devices[6]  = (hc_device_t){"厨房灯", false, false};
  s_hc_devices[7]  = (hc_device_t){"玄关灯",     false, false};
  s_hc_devices[8]  = (hc_device_t){"卧室灯",     false, false};
  s_hc_devices[9]  = (hc_device_t){"卧室背景灯", false, false};
  s_hc_devices[10] = (hc_device_t){"卫生间灯",   false, false};
  s_hc_devices[11] = (hc_device_t){"窗帘",       false, true};

  s_hc_devices_initialized = true;
}

/**
 * @brief 通过 UDP 广播发现控制服务器
 *
 *  向局域网广播 DISCOVER_REQ，等待服务器回复
 *  "DISCOVER_RESP <IP> <PORT>"，解析后填充服务器信息。
 */
static void hc_discover_server(void)
{
  HC_LOG("Discovering server via UDP broadcast...");

  memset(s_hc_server_ip, 0, sizeof(s_hc_server_ip));
  s_hc_server_port = 0;
  s_hc_server_discovered = false;

  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      HC_LOG("Failed to create socket");
      return;
    }

  /* 绑定到 wlan0 接口，确保从正确接口广播 */
  struct in_addr local_ip;
  if (netlib_get_ipv4addr("wlan0", &local_ip) == 0 &&
      local_ip.s_addr != 0)
    {
      HC_LOG("Local wlan0 IP: %s", inet_ntoa(local_ip));

      struct sockaddr_in local_addr;
      memset(&local_addr, 0, sizeof(local_addr));
      local_addr.sin_family = AF_INET;
      local_addr.sin_addr   = local_ip;
      local_addr.sin_port   = 0;

      if (bind(sockfd, (struct sockaddr *)&local_addr,
               sizeof(local_addr)) < 0)
        {
          HC_LOG("Failed to bind to wlan0 interface");
        }
    }
  else
    {
      HC_LOG("Failed to get wlan0 IP address");
    }

  /* 设置广播选项 */
  int broadcast = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST,
             &broadcast, sizeof(broadcast));

  /* 发送发现请求 */
  struct sockaddr_in broadcast_addr;
  memset(&broadcast_addr, 0, sizeof(broadcast_addr));
  broadcast_addr.sin_family      = AF_INET;
  broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;
  broadcast_addr.sin_port        = htons(HC_DISCOVERY_PORT);

  int ret = sendto(sockfd, HC_DISCOVERY_MAGIC,
                   strlen(HC_DISCOVERY_MAGIC), 0,
                   (struct sockaddr *)&broadcast_addr,
                   sizeof(broadcast_addr));
  if (ret < 0)
    {
      HC_LOG("Broadcast send failed");
      close(sockfd);
      return;
    }

  /* 等待响应（超时 5 秒） */
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(sockfd, &readfds);

  struct timeval timeout;
  timeout.tv_sec  = HC_DISCOVERY_TIMEOUT;
  timeout.tv_usec = 0;

  ret = select(sockfd + 1, &readfds, NULL, NULL, &timeout);
  if (ret <= 0)
    {
      HC_LOG("Discovery timeout or error");
      close(sockfd);
      return;
    }

  /* 接收响应 */
  struct sockaddr_in server_addr;
  socklen_t server_len = sizeof(server_addr);
  char buffer[256];

  ret = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                 (struct sockaddr *)&server_addr, &server_len);
  if (ret < 0)
    {
      HC_LOG("Receive response failed");
      close(sockfd);
      return;
    }

  buffer[ret] = '\0';

  /* 解析响应：DISCOVER_RESP <IP> <PORT> */
  if (strncmp(buffer, HC_RESPONSE_MAGIC,
              strlen(HC_RESPONSE_MAGIC)) == 0)
    {
      char *ip_start   = buffer + strlen(HC_RESPONSE_MAGIC) + 1;
      char *port_start = strchr(ip_start, ' ');

      if (port_start != NULL)
        {
          *port_start = '\0';
          port_start++;

          strncpy(s_hc_server_ip, ip_start,
                  sizeof(s_hc_server_ip) - 1);
          s_hc_server_port      = atoi(port_start);
          s_hc_server_discovered = true;

          HC_LOG("Server discovered: %s:%d",
                 s_hc_server_ip, s_hc_server_port);
          close(sockfd);
          return;
        }
    }

  HC_LOG("Invalid response format");
  close(sockfd);
}

/**
 * @brief 通过 TCP 短连接发送控制命令
 *
 * @param cmd 命令编号
 * @return 成功返回发送字节数，失败返回 -1
 */
static int hc_send_command(int cmd)
{
  if (!s_hc_server_discovered)
    {
      HC_LOG("Server not discovered, cannot send command");
      return -1;
    }

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    {
      HC_LOG("Failed to create socket");
      return -1;
    }

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port   = htons(s_hc_server_port);
  inet_pton(AF_INET, s_hc_server_ip, &server_addr.sin_addr);

  if (connect(sock, (struct sockaddr *)&server_addr,
              sizeof(server_addr)) < 0)
    {
      HC_LOG("Failed to connect to %s:%d",
             s_hc_server_ip, s_hc_server_port);
      close(sock);
      return -1;
    }

  char cmd_str[16] = {0};
  snprintf(cmd_str, sizeof(cmd_str), "%d", cmd);

  int ret = send(sock, cmd_str, strlen(cmd_str), 0);

  if (ret < 0)
    {
      HC_LOG("Failed to send command %d", cmd);
    }
  else
    {
      HC_LOG("Sent command %d to %s:%d",
             cmd, s_hc_server_ip, s_hc_server_port);
    }

  close(sock);
  return ret;
}

/**
 * @brief 计算设备控制命令编号
 *
 *  普通设备：开 = index*2+1，关 = index*2+2
 *  窗帘（带子开关）：开 = 23（上电），关 = 24（断电）
 */
static int hc_calc_command(int device_index, bool turn_on)
{
  if (s_hc_devices[device_index].has_sub_switch)
    {
      return turn_on ? 23 : 24;
    }
  return turn_on ? (device_index * 2 + 1) : (device_index * 2 + 2);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void watch_home_control_reset_server(void)
{
  memset(s_hc_server_ip, 0, sizeof(s_hc_server_ip));
  s_hc_server_port      = 0;
  s_hc_server_discovered = false;
  HC_LOG("Server info reset");
}

int watch_home_control_get_device_count(void)
{
  return HC_DEVICE_COUNT;
}

const char *watch_home_control_get_device_name(int index)
{
  if (index < 0 || index >= HC_DEVICE_COUNT)
    {
      return NULL;
    }
  hc_init_devices();
  return s_hc_devices[index].name;
}

bool watch_home_control_get_device_status(int index)
{
  if (index < 0 || index >= HC_DEVICE_COUNT)
    {
      return false;
    }
  hc_init_devices();
  return s_hc_devices[index].status;
}

int watch_home_control_toggle_device(int index, bool turn_on)
{
  if (index < 0 || index >= HC_DEVICE_COUNT)
    {
      HC_LOG("Invalid device index %d", index);
      return -1;
    }

  hc_init_devices();

  /* WiFi 前置检查 */
  if (!settings_wifi_is_connected())
    {
      HC_LOG("WiFi not connected, please connect WiFi first");
      return -2;
    }

  /* 服务器发现（首次或重置后） */
  if (!s_hc_server_discovered)
    {
      HC_LOG("Server not discovered, discovering...");
      hc_discover_server();
    }

  if (!s_hc_server_discovered)
    {
      HC_LOG("Server discovery failed");
      return -3;
    }

  /* 计算命令编号并发送 */
  int cmd = hc_calc_command(index, turn_on);
  HC_LOG("Sending command %d for device %s (%s)",
         cmd, s_hc_devices[index].name, turn_on ? "ON" : "OFF");

  int ret = hc_send_command(cmd);
  if (ret < 0)
    {
      return -4;
    }

  /* 更新设备状态 */
  s_hc_devices[index].status = turn_on;
  HC_LOG("Device %s set to %s",
         s_hc_devices[index].name, turn_on ? "ON" : "OFF");

  return 0;
}
