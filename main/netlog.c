/*
 * UDP 网络日志（端口 5140 广播）。
 * 铁律：vprintf 钩子可能在任何任务上下文被调（包括 lwip/wifi 内部），
 * 绝不能在钩子里直接 sendto——那会跟 tcpip 线程互等而死锁。
 * 钩子只做无阻塞入队，发送由独立任务完成；队列满就丢日志。
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#define MSG_LEN 200
typedef struct { uint16_t len; char s[MSG_LEN]; } logmsg_t;

static QueueHandle_t  s_q;
static vprintf_like_t s_prev;

static int net_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = s_prev ? s_prev(fmt, ap) : 0;
    if (s_q) {
        logmsg_t m;
        int k = vsnprintf(m.s, MSG_LEN, fmt, ap2);
        if (k > 0) {
            m.len = k > MSG_LEN ? MSG_LEN : k;
            xQueueSend(s_q, &m, 0);          /* 满了就丢，绝不阻塞 */
        }
    }
    va_end(ap2);
    return n;
}

static void sender_task(void *a)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int bc = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bc, sizeof bc);
    /* 路由器会滤掉 255.255.255.255 广播——单播直发开发机 */
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(5140) };
    dst.sin_addr.s_addr = inet_addr("192.168.3.237");
    logmsg_t m;
    while (1) {
        if (xQueueReceive(s_q, &m, portMAX_DELAY) == pdTRUE)
            sendto(fd, m.s, m.len, 0, (struct sockaddr *)&dst, sizeof dst);
    }
}

void netlog_start(void)
{
    s_q = xQueueCreate(64, sizeof(logmsg_t));
    xTaskCreate(sender_task, "netlog", 3072, NULL, 2, NULL);
    s_prev = esp_log_set_vprintf(net_vprintf);
}
