
#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <nuttx/wireless/wireless.h>
#include "esp32s3_wifi_adapter.h"
#include <net/route.h>
#include "netutils/netlib.h"
#include <ctype.h>
#include <math.h>
#include <net/if_arp.h>
/* 网络服务器配置 */
#define SERVER_PORT 8080              /* TCP服务器端口 */
#define MAX_CLIENTS 5                 /* 最大客户端数量 */
#define BUFFER_SIZE 1024              /* 缓冲区大小 */
#define HEARTBEAT_INTERVAL 5          /* 心跳间隔（秒） */
/* 客户端结构体 */
typedef struct {
  int socket;                        /* 客户端socket */
  struct sockaddr_in addr;           /* 客户端地址 */
  int active;                        /* 客户端状态 */
} client_t;
/* 全局变量 */
static client_t g_clients[MAX_CLIENTS];  /* 客户端列表 */
static int g_client_count = 0;          /* 当前客户端数量 */
static int g_server_fd = -1;            /* 服务器socket */
/****************************************************************************
 * 函数声明
 ****************************************************************************/
static int wifi_init(void);
static int wifi_reconnect_task(int argc, char **argv);
static int start_server(void);
static void handle_clients(void);
static void send_heartbeat(void);
static void handle_client_message(int client_idx);
static void print_network_info(void);
static int wifi_scan(void);
static int wifi_connect_with_ssid(const char *ssid, const char *password);
/****************************************************************************
 * 主函数
 ****************************************************************************/
int main(int argc, FAR char *argv[])
{
  int ret;
  int pid;
  (void)argc;  /* 未使用的参数 */
  (void)argv;  /* 未使用的参数 */
  printf("ESP32S3 WiFi 示例程序\n");
  printf("====================================\n");
  printf("OpenVela 框架 - ESP32S3 WiFi Station\n");
  printf("====================================\n");
  /* 初始化客户端列表 */
  memset(g_clients, 0, sizeof(g_clients));
  /* 初始化WiFi */
  ret = wifi_init();
  if (ret != 0)
    {
      printf("WiFi初始化失败: %d\n", ret);
      return ret;
    }
  /* 扫描WiFi网络并连接 */
  ret = wifi_scan();
  if (ret != 0)
    {
      printf("WiFi扫描和连接失败: %d\n", ret);
      return ret;
    }
  /* 打印网络信息 */
  print_network_info();
  
  /* 启动TCP服务器 */
  ret = start_server();
  if (ret != 0)
    {
      printf("服务器启动失败: %d\n", ret);
      return ret;
    }
  /* 启动WiFi重连任务 */
  pid = task_create("wifi_reconnect", 100, 2048, (main_t)wifi_reconnect_task, NULL);
  if (pid < 0)
    {
      printf("创建WiFi重连任务失败\n");
    }
  /* 主循环 */
  int loop_count = 0;
  while (1)
    {
      /* 处理客户端连接和消息 */
      handle_clients();
      
      /* 发送心跳消息 */
      send_heartbeat();
      
      /* 定期输出状态信息 */
      if (++loop_count % 50 == 0)  /* 每5秒输出一次状态 */
        {
          printf("服务器状态: 活跃连接数=%d, 循环次数=%d\n", g_client_count, loop_count);
          if(loop_count > 3000)
          {
            return 0;
          }
        }
      
      /* 短暂休眠 */
      usleep(100000); // 100ms
    }
  return 0;
}
/****************************************************************************
 * WiFi初始化
 ****************************************************************************/
static int wifi_init(void)
{
  int ret;
  printf("正在初始化WiFi适配器...\n");
  /* 初始化WiFi适配器 */
  ret = esp_wifi_adapter_init();
  if (ret != 0)
    {
      printf("WiFi适配器初始化失败: %d\n", ret);
      return ret;
    }
  printf("WiFi适配器初始化成功\n");
  return 0;
}
/****************************************************************************
 * WiFi连接
 ****************************************************************************/
static int wifi_reconnect_task(int argc, char **argv)
{
  (void)argc;  /* 未使用的参数 */
  (void)argv;  /* 未使用的参数 */
  while (1)
    {
      /* 检查WiFi连接状态 */
      /* 这里可以添加WiFi状态检查逻辑 */
      
      /* 每30秒检查一次 */
      sleep(30);
    }
  return 0;
}
/****************************************************************************
 * 启动TCP服务器
 ****************************************************************************/
static int start_server(void)
{
  struct sockaddr_in address;
  int opt = 1;
  printf("正在启动TCP服务器，端口: %d...\n", SERVER_PORT);
  /* 创建socket文件描述符 */
  if ((g_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
      perror("创建socket失败");
      return -1;
    }
  /* 设置socket选项 */
  if (setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
      perror("设置socket选项失败");
      close(g_server_fd);
      return -1;
    }
  /* 配置地址 */
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(SERVER_PORT);
  /* 绑定socket到端口 */
  if (bind(g_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
      perror("绑定失败");
      close(g_server_fd);
      return -1;
    }
  /* 开始监听 */
  if (listen(g_server_fd, MAX_CLIENTS) < 0)
    {
      perror("监听失败");
      close(g_server_fd);
      return -1;
    }
  printf("服务器启动成功，等待连接...\n");
  
  /* 输出网络接口信息 */
  printf("网络接口状态:\n");
  printf("  监听地址: 0.0.0.0:%d\n", SERVER_PORT);
  printf("  Socket FD: %d\n", g_server_fd);
  
  return 0;
}
/****************************************************************************
 * 处理客户端连接和消息
 ****************************************************************************/
static void handle_clients(void)
{
  struct sockaddr_in address;
  int new_socket;
  fd_set readfds;
  struct timeval timeout;
  /* 设置文件描述符集合 */
  FD_ZERO(&readfds);
  FD_SET(g_server_fd, &readfds);
  int max_fd = g_server_fd;
  /* 添加客户端socket到集合 */
  for (int i = 0; i < MAX_CLIENTS; i++)
    {
      if (g_clients[i].active)
        {
          FD_SET(g_clients[i].socket, &readfds);
          if (g_clients[i].socket > max_fd)
            {
              max_fd = g_clients[i].socket;
            }
        }
    }
  /* 设置超时时间 */
  timeout.tv_sec = 0;
  timeout.tv_usec = 100000; // 100ms
  /* 等待活动 */
  int activity = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
  if (activity < 0)
    {
      perror("select失败");
      return;
    }
  else if (activity == 0)
    {
      /* 超时，正常情况 */
      return;
    }
  /* 检查新连接 */
  if (FD_ISSET(g_server_fd, &readfds))
    {
      socklen_t addr_len = sizeof(address);
      new_socket = accept(g_server_fd, (struct sockaddr *)&address, &addr_len);
      if (new_socket < 0)
        {
          perror("接受连接失败");
          return;
        }
      printf("新连接来自: %s:%d\n", inet_ntoa(address.sin_addr), ntohs(address.sin_port));
      /* 找到空闲客户端槽位 */
      int i;
      for (i = 0; i < MAX_CLIENTS; i++)
        {
          if (!g_clients[i].active)
            {
              g_clients[i].socket = new_socket;
              g_clients[i].addr = address;
              g_clients[i].active = 1;
              g_client_count++;
              printf("客户端已连接，当前连接数: %d\n", g_client_count);
              break;
            }
        }
      if (i == MAX_CLIENTS)
        {
          printf("客户端连接数达到上限\n");
          close(new_socket);
        }
    }
  /* 检查客户端消息 */
  for (int i = 0; i < MAX_CLIENTS; i++)
    {
      if (g_clients[i].active && FD_ISSET(g_clients[i].socket, &readfds))
        {
          handle_client_message(i);
        }
    }
}
/****************************************************************************
 * 处理客户端消息
 ****************************************************************************/
static void handle_client_message(int client_idx)
{
  char buffer[BUFFER_SIZE] = {0};
  int valread = read(g_clients[client_idx].socket, buffer, BUFFER_SIZE);
  if (valread == 0)
    {
      /* 客户端断开连接 */
      printf("客户端断开连接: %s:%d\n", 
             inet_ntoa(g_clients[client_idx].addr.sin_addr), 
             ntohs(g_clients[client_idx].addr.sin_port));
      close(g_clients[client_idx].socket);
      g_clients[client_idx].active = 0;
      g_client_count--;
      printf("当前连接数: %d\n", g_client_count);
    }
  else if (valread > 0)
    {
      /* 收到客户端消息 */
      printf("收到消息: %s\n", buffer);
      /* 准备响应消息 */
      char response[BUFFER_SIZE];
      snprintf(response, BUFFER_SIZE, "ESP32S3收到: %.990s", buffer);
      send(g_clients[client_idx].socket, response, strlen(response), 0);
      printf("发送响应: %s\n", response);
    }
}
/****************************************************************************
 * 发送心跳消息
 ****************************************************************************/
static void send_heartbeat(void)
{
  static int count = 0;
  
  if (++count >= HEARTBEAT_INTERVAL * 10) // 10次循环为1秒
    {
      count = 0;
      
      /* 向所有活跃客户端发送心跳消息 */
      for (int i = 0; i < MAX_CLIENTS; i++)
        {
          if (g_clients[i].active)
            {
              char heartbeat[BUFFER_SIZE];
              snprintf(heartbeat, BUFFER_SIZE, "ESP32S3心跳 - %d", (int)time(NULL));
              int ret = send(g_clients[i].socket, heartbeat, strlen(heartbeat), 0);
              if (ret < 0)
                {
                  /* 发送失败，客户端可能已断开 */
                  printf("发送心跳失败，客户端已断开\n");
                  close(g_clients[i].socket);
                  g_clients[i].active = 0;
                  g_client_count--;
                }
            }
        }
    }
}
/****************************************************************************
 * 打印IP地址
 ****************************************************************************/
static void print_network_info(void)
{
  int sockfd;
  struct ifreq ifr;
  char ip[INET_ADDRSTRLEN];
  char netmask[INET_ADDRSTRLEN];
  char broadcast[INET_ADDRSTRLEN];
  /* 创建socket */
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      perror("创建socket失败");
      return;
    }
  /* 获取wlan0接口的IP地址 */
  strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
  if (ioctl(sockfd, SIOCGIFADDR, &ifr) < 0)
    {
      perror("获取IP地址失败");
      close(sockfd);
      return;
    }
  /* 转换为字符串 */
  inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, ip, INET_ADDRSTRLEN);
  printf("====================================\n");
  printf("WiFi连接成功！\n");
  printf("IP地址: %s\n", ip);
  printf("TCP服务器端口: %d\n", SERVER_PORT);
  printf("====================================\n");
  printf("请在手机上连接同一WiFi网络，然后使用网络调试助手\n");
  printf("连接到: %s:%d\n", ip, SERVER_PORT);
  printf("====================================\n");
  /* 检查wlan0接口状态 */
  strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
  if (ioctl(sockfd, SIOCGIFFLAGS, &ifr) >= 0)
    {
      printf("网络接口状态:\n");
      printf("  接口: wlan0\n");
      printf("  状态: %s\n", (ifr.ifr_flags & IFF_UP) ? "UP" : "DOWN");
      printf("  运行: %s\n", (ifr.ifr_flags & IFF_RUNNING) ? "RUNNING" : "NOT RUNNING");
    }
  
  /* 检查网络掩码 */
  if (ioctl(sockfd, SIOCGIFNETMASK, &ifr) == 0)
    {
      inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr, netmask, INET_ADDRSTRLEN);
      printf("  子网掩码: %s\n", netmask);
    }
  
  /* 检查广播地址 */
  if (ioctl(sockfd, SIOCGIFBRDADDR, &ifr) == 0)
    {
      inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_broadaddr)->sin_addr, broadcast, INET_ADDRSTRLEN);
      printf("  广播地址: %s\n", broadcast);
    }
  close(sockfd);
}
/****************************************************************************
 * 扫描WiFi网络
 ****************************************************************************/
/* 扫描信息结构体 */
typedef struct {
  char ssid[33];          /* WiFi名称 */
  int rssi;              /* 信号强度 (dBm) */
  bool has_rssi;         /* 是否有信号强度 */
  int freq;              /* 频率 */
  bool has_freq;         /* 是否有频率 */
  int bitrate;           /* 比特率 */
  bool has_bitrate;      /* 是否有比特率 */
  int encode;            /* 加密方式 */
  bool has_encode;       /* 是否有加密方式 */
} wifi_network_info_t;
/* 事件流处理结构体 */
typedef struct {
  char *end;                  /* 流的结束 */
  char *current;              /* 流中的当前事件 */
} event_stream_t;
/* 初始化事件流 */
static void event_stream_init(event_stream_t *stream, char *data, size_t len)
{
  memset(stream, 0, sizeof(event_stream_t));
  stream->current = data;
  stream->end = &data[len];
}
/* 从流中提取下一个事件 */
static int event_stream_extract(event_stream_t *stream, struct iw_event *iwe)
{
  struct iw_event *iwe_stream;
  iwe_stream = (struct iw_event *)stream->current;
  if (stream->current + offsetof(struct iw_event, u) > stream->end ||
      iwe_stream->len == 0)
    {
      return 0; /* 没有更多事件 */
    }
  if (stream->current + iwe_stream->len > stream->end ||
      iwe_stream->len < offsetof(struct iw_event, u))
    {
      return -EINVAL; /* 无效事件 */
    }
  switch (iwe_stream->cmd)
    {
      case SIOCGIWESSID:
      case SIOCGIWENCODE:
        iwe->cmd = iwe_stream->cmd;
        iwe->len = offsetof(struct iw_event, u) + sizeof(struct iw_point);
        iwe->u.data.flags = iwe_stream->u.data.flags;
        iwe->u.data.length = iwe_stream->u.data.length;
        iwe->u.data.pointer = (void *)(stream->current +
                              offsetof(struct iw_event, u) +
                              (unsigned long)iwe_stream->u.data.pointer);
        break;
      default:
        if (iwe_stream->len > sizeof(*iwe))
          {
            printf("未处理的事件大小: 0x%x %d\n", iwe_stream->cmd, iwe_stream->len);
            iwe->cmd = 0;
            iwe->len = offsetof(struct iw_event, u);
            break;
          }
        memcpy(iwe, iwe_stream, iwe_stream->len);
    }
  /* 更新流到下一个事件 */
  stream->current += iwe_stream->len;
  return 1; /* 成功提取事件 */
}
/* 处理扫描事件 */
static int process_scan_event(struct iw_event *event, wifi_network_info_t *info)
{
  switch (event->cmd)
    {
      case SIOCGIWESSID:
        {
          memset(info->ssid, 0, sizeof(info->ssid));
          if ((event->u.essid.pointer) && (event->u.essid.length))
            {
              memcpy(info->ssid, event->u.essid.pointer,
                     event->u.essid.length);
              info->ssid[event->u.essid.length] = '\0';
            }
          break;
        }
      case SIOCGIWFREQ:
        {
          info->has_freq = true;
          if (event->u.freq.e == 0)
            {
              /* 有些驱动报告的是信道而不是频率 */
              if (event->u.freq.m >= 1 && event->u.freq.m <= 13)
                {
                  info->freq = 2407 + 5 * event->u.freq.m;
                }
              else if (event->u.freq.m == 14)
                {
                  info->freq = 2484;
                }
              else if (event->u.freq.m >= 36 && event->u.freq.m <= 165)
                {
                  info->freq = 5000 + 5 * event->u.freq.m;
                }
            }
          else
            {
              /* 转换频率 */
              info->freq = (int)((double)event->u.freq.m * pow(10, event->u.freq.e));
            }
          break;
        }
      case SIOCGIWRATE:
        {
          /* 只保留最大的比特率 */
          if (!info->has_bitrate || event->u.bitrate.value > info->bitrate)
            {
              info->has_bitrate = true;
              info->bitrate = event->u.bitrate.value;
            }
          break;
        }
      case IWEVQUAL:
        {
          if (event->u.qual.updated & IW_QUAL_DBM)
            {
              info->has_rssi = true;
              info->rssi = event->u.qual.level;
              /* 报告dBm格式的信号强度 */
              if (info->rssi >= 0x40)
                {
                  info->rssi -= 0x100;
                }
            }
          break;
        }
      case SIOCGIWENCODE:
        {
          info->has_encode = true;
          info->encode = event->u.data.flags;
          break;
        }
    }
  return 0;
}
static int wifi_scan(void)
{
  int sockfd;
  int ret;
  char *scan_buffer = NULL;
  size_t scan_buffer_size = 4096; /* 初始缓冲区大小 */
  int choice;
  char ssid[33];
  char password[65];
  wifi_network_info_t networks[20]; /* 最多保存20个网络 */
  int valid_network_count = 0;
  printf("\n开始扫描WiFi网络...\n");
  /* 创建socket */
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      printf("创建socket失败: %d\n", errno);
      return -errno;
    }
  /* 开始扫描 - 使用与wapi相同的方法 */
  struct iw_scan_req req;
  struct iwreq iwr;
  memset(&req, 0, sizeof(req));
  req.scan_type = IW_SCAN_TYPE_ACTIVE;
  req.bssid.sa_family = ARPHRD_ETHER;
  memset(req.bssid.sa_data, 0xff, IFHWADDRLEN);
  memset(&iwr, 0, sizeof(iwr));
  strncpy(iwr.ifr_name, "wlan0", IFNAMSIZ);
  iwr.u.data.pointer = (caddr_t)&req;
  iwr.u.data.length = sizeof(req);
  ret = ioctl(sockfd, SIOCSIWSCAN, &iwr);
  if (ret < 0)
    {
      printf("开始扫描失败: %d\n", errno);
      close(sockfd);
      return -errno;
    }
  /* 等待扫描完成 - 与wapi相同的状态检查 */
  printf("正在扫描...\n");
  int scan_status;
  int retry_count = 0;
  const int max_retry = 10;
  do
    {
      usleep(1000000); /* 等待1秒 */
      scan_status = ioctl(sockfd, SIOCGIWSCAN, &iwr);
      retry_count++;
    } while (scan_status < 0 && errno == EAGAIN && retry_count < max_retry);
  if (retry_count >= max_retry)
    {
      printf("扫描超时\n");
      close(sockfd);
      return -ETIMEDOUT;
    }
  /* 分配缓冲区并获取扫描结果 */
  scan_buffer = (char *)malloc(scan_buffer_size);
  if (!scan_buffer)
    {
      printf("分配缓冲区失败\n");
      close(sockfd);
      return -ENOMEM;
    }
retry:
  memset(scan_buffer, 0, scan_buffer_size);
  iwr.u.data.pointer = scan_buffer;
  iwr.u.data.length = scan_buffer_size;
  iwr.u.data.flags = 0;
  ret = ioctl(sockfd, SIOCGIWSCAN, &iwr);
  if (ret < 0 && errno == E2BIG)
    {
      /* 缓冲区太小，重新分配 */
      char *tmp;
      scan_buffer_size *= 2;
      tmp = (char *)realloc(scan_buffer, scan_buffer_size);
      if (!tmp)
        {
          printf("重新分配缓冲区失败\n");
          free(scan_buffer);
          close(sockfd);
          return -ENOMEM;
        }
      scan_buffer = tmp;
      goto retry;
    }
  if (ret < 0)
    {
      printf("获取扫描结果失败: %d\n", errno);
      free(scan_buffer);
      close(sockfd);
      return -errno;
    }
  /* 解析扫描结果 */
  printf("\n扫描到的WiFi网络:\n");
  printf("---------------------------------------------------\n");
  printf("  #  SSID                信号强度  频率\n");
  printf("---------------------------------------------------\n");
  if (iwr.u.data.length > 0)
    {
      struct iw_event iwe;
      event_stream_t stream;
      wifi_network_info_t current_network;
      bool in_network = false;
      event_stream_init(&stream, scan_buffer, iwr.u.data.length);
      
      do
        {
          ret = event_stream_extract(&stream, &iwe);
          if (ret > 0)
            {
              /* 处理SIOCGIWAP事件 - 新网络的开始 */
              if (iwe.cmd == SIOCGIWAP)
                {
                  if (in_network && valid_network_count < 20)
                    {
                      /* 保存当前网络信息 */
                      networks[valid_network_count++] = current_network;
                    }
                  /* 初始化新网络 */
                  memset(&current_network, 0, sizeof(current_network));
                  in_network = true;
                }
              else if (in_network)
                {
                  /* 处理网络相关事件 */
                  process_scan_event(&iwe, &current_network);
                }
            }
        } while (ret > 0);
      /* 保存最后一个网络 */
      if (in_network && valid_network_count < 20)
        {
          networks[valid_network_count++] = current_network;
        }
    }
  /* 显示扫描结果 */
  if (valid_network_count == 0)
    {
      printf("  未扫描到WiFi网络\n");
      free(scan_buffer);
      close(sockfd);
      return -1;
    }
  
  for (int i = 0; i < valid_network_count; i++)
    {
      wifi_network_info_t *net = &networks[i];
      
      /* 确保SSID是有效的字符串 */
      if (strlen(net->ssid) > 0)
        {
          /* 打印编号、SSID和信号强度，限制SSID长度为20个字符 */
          char ssid_display[21];
          int len = strlen(net->ssid);
          if (len > 20)
            {
              strncpy(ssid_display, net->ssid, 17);
              strcpy(ssid_display + 17, "...");
              ssid_display[20] = '\0';
            }
          else
            {
              strncpy(ssid_display, net->ssid, 20);
              ssid_display[20] = '\0';
            }
          
          /* 打印信号强度，如果有的话 */
          if (net->has_rssi)
            {
              printf("  %-2d %-20s %-8d dBm  %4d MHz\n", 
                     i+1, ssid_display, net->rssi, 
                     net->has_freq ? net->freq : 0);
            }
          else
            {
              printf("  %-2d %-20s %-8s  %4d MHz\n", 
                     i+1, ssid_display, "N/A", 
                     net->has_freq ? net->freq : 0);
            }
        }
    }
  printf("---------------------------------------------------\n");
  /* 让用户选择WiFi */
  printf("请选择要连接的WiFi编号 (1-%d): \n", valid_network_count);
  scanf("%d", &choice);
  while (getchar()!='\n' && getchar()!=EOF);
  if(choice == 0)
    {
      printf("用户取消选择\n");
      return 0;
    }
  while (choice < 1 || choice > valid_network_count)
    {
      printf("输入无效，请重新选择 (1-%d): \n", valid_network_count);
      scanf("%d", &choice);
      while (getchar()!='\n' && getchar()!=EOF);
    }
  /* 根据选择设置SSID */
  strncpy(ssid, networks[choice-1].ssid, sizeof(ssid)-1);
  ssid[sizeof(ssid)-1] = '\0';
  printf("选择的wifi: %s\n", ssid);
  /* 输入密码 */
  printf("请输入WiFi密码: \n");
  scanf("%s", password);
  /* 连接选择的WiFi */
  ret = wifi_connect_with_ssid(ssid, password);
  /* 清理 */
  free(scan_buffer);
  close(sockfd);
  return ret;
}
/****************************************************************************
 * 连接指定SSID的WiFi - 使用ESP32特定的API
 ****************************************************************************/
static int wifi_connect_with_ssid(const char *ssid, const char *password)
{
  int ret;
  struct iwreq iwr;
  char ssid_buf[32];
  char password_buf[64];
  printf("\n正在连接WiFi: %s\n", ssid);
  /* 设置SSID */
  memset(&iwr, 0, sizeof(iwr));
  strncpy(ssid_buf, ssid, sizeof(ssid_buf) - 1);
  iwr.u.essid.pointer = (FAR void *)ssid_buf;
  iwr.u.essid.length = strlen(ssid);
  iwr.u.essid.flags = 0;
  ret = esp_wifi_sta_essid(&iwr, true);
  if (ret != 0)
    {
      printf("设置SSID失败: %d\n", ret);
      return ret;
    }
  /* 设置密码 */
  memset(&iwr, 0, sizeof(iwr));
  strncpy(password_buf, password, sizeof(password_buf) - 1);
  
  /* 创建并设置iw_encode_ext结构 */
  struct iw_encode_ext *ext = (struct iw_encode_ext *)malloc(sizeof(struct iw_encode_ext) + strlen(password));
  if (!ext)
    {
      printf("内存分配失败\n");
      return -ENOMEM;
    }
  
  memset(ext, 0, sizeof(struct iw_encode_ext));
  ext->ext_flags = 0;
  ext->alg = IW_ENCODE_ALG_CCMP; /* 使用WPA2加密 */
  ext->key_len = strlen(password);
  memcpy(ext->key, password_buf, ext->key_len);
  
  iwr.u.encoding.pointer = (FAR void *)ext;
  iwr.u.encoding.length = sizeof(struct iw_encode_ext) + ext->key_len;
  iwr.u.encoding.flags = 0;
  
  ret = esp_wifi_sta_password(&iwr, true);
  if (ret != 0)
    {
      printf("设置密码失败: %d\n", ret);
      free(ext);
      return ret;
    }
  
  free(ext);
  /* 启动station模式 */
  ret = esp_wifi_sta_start();
  if (ret != 0)
    {
      printf("启动station模式失败: %d\n", ret);
      return ret;
    }
  /* 连接到AP */
  ret = esp_wifi_sta_connect();
  if (ret != 0)
    {
      printf("连接到AP失败: %d\n", ret);
      return ret;
    }
  /* 等待连接建立 */
  printf("正在连接...");
  fflush(stdout);
  for (int i = 0; i < 15; i++)
    {
      sleep(1);
      printf(".");
      fflush(stdout);
    }
  printf("\n");
  /* 调用DHCP客户端获取IP地址 */
  printf("正在获取IP地址...\n");
  ret = netlib_obtain_ipv4addr("wlan0");
  if (ret < 0)
    {
      printf("获取IP地址失败: %d\n", ret);
      return ret;
    }
  printf("IP地址获取成功!\n");
  return 0;
}
