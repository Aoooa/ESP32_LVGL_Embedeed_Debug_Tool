/* usbip_server.c —— 无线 DAP 通道：USB/IP server（TCP 872）
 *
 * 协议流程（参考 windowsair/wireless-esp8266-dap，MIT）：
 *   stage1: OP_REQ_DEVLIST（设备列表）/ OP_REQ_IMPORT（attach 握手）
 *   stage2: URB 循环（USBIP_CMD_SUBMIT 48B 头）：
 *     - ep0 控制：GET_DESCRIPTOR（DEVICE/CONFIG/STRING/HID Report 复用 USB 直连
 *       描述符）；SET_REPORT = DAP 命令（wIndex = 接口号 = SWD 端口）
 *     - ep1/ep2 中断 IN：HID 轮询，响应就绪时立即回，否则挂起 1 个 URB
 *       等 SET_REPORT 产出响应后补回（Windows HID 驱动每端点同时只挂 1 个）
 *     - UNLINK：零状态回（CMSIS-DAP 会话中 Windows 不取消 HID 轮询）
 *
 * 线程：独立任务，单连接串行处理（usbip-win 单客户端）。
 * 互斥：DAP 内核与 USB 通道共用，经 dap_port_locks[]（app_dap.c）串行。 */

#include "usbip_server.h"
#include "usbip_defs.h"
#include "app_dap.h"
#include "drv_dap.h"
#include "tusb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>

static const char *TAG = "usbip_server";

/* 每端口中断 IN 挂起槽（单 host 单 URB，够用） */
typedef struct {
    bool pending;        /* 有挂起的 IN URB 等响应 */
    uint32_t seqnum;     /* 挂起 URB 的 seqnum */
    bool data_ready;     /* 有就绪响应待发送 */
    uint8_t data[64];
    uint16_t data_len;
} epin_slot_t;

static void epin_flush(int port, int c);   /* handle_control 中补回挂起 IN URB */

static TaskHandle_t s_task;
static int s_sock = -1;
static bool s_running;
static epin_slot_t s_epin[DAP_PORT_COUNT];
static SemaphoreHandle_t s_epin_mutex;

/* ── 网络收发 ── */

static int recv_exact(int c, void *buf, int len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        int n = recv(c, p, len, 0);
        if (n <= 0) {
            ESP_LOGW(TAG, "recv failed: want %d got %d", len, n);
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int send_all(int c, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = send(c, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

/* ── stage2 响应 ── */

static void send_ret_submit(int c, const usbip_stage2_header *req, int32_t status,
                            int32_t data_len, const void *data)
{
    usbip_stage2_header rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.base.command  = htonl(USBIP_RET_SUBMIT);
    rsp.base.seqnum   = req->base.seqnum;   /* 原样回（网络序） */
    rsp.base.devid    = req->base.devid;
    /* USBIP 协议：响应方向必须与请求相反（stub 端反转） */
    rsp.base.direction = htonl(ntohl(req->base.direction) == USBIP_DIR_IN
                               ? USBIP_DIR_OUT : USBIP_DIR_IN);
    rsp.base.ep       = req->base.ep;
    rsp.u.ret_submit.status      = htonl(status);
    rsp.u.ret_submit.data_length = htonl(data_len);
    if (send_all(c, &rsp, USBIP_STAGE2_HDR_SIZE) < 0) return;
    if (data_len > 0 && data) send_all(c, data, data_len);
}

/* ── ep0 控制请求 ── */

/* 字符串描述符应答（复用 USB 直连字符串表；s_serial_str 为运行时数据） */
static int string_to_desc(int idx, uint8_t *out, int maxlen)
{
    const char *s = NULL;
    if (idx == 0) {
        out[0] = 4; out[1] = 0x03; out[2] = 0x09; out[3] = 0x04;
        return 4;
    }
    s = dap_usb_string(idx);
    if (!s) return 0;
    int n = (int)strlen(s);
    if (n > (maxlen - 2) / 2) n = (maxlen - 2) / 2;
    int len = 2 + n * 2;
    out[0] = (uint8_t)len;
    out[1] = 0x03;
    for (int i = 0; i < n; i++) {
        out[2 + i * 2] = (uint8_t)s[i];
        out[3 + i * 2] = 0;
    }
    return len;
}

static void handle_control(int c, usbip_stage2_header *h, const uint8_t *setup,
                           const uint8_t *data_in, int data_len)
{
    uint8_t bmReqType = setup[0];
    uint8_t bRequest  = setup[1];
    uint16_t wValue   = (uint16_t)(setup[2] | (setup[3] << 8));
    uint16_t wIndex   = (uint16_t)(setup[4] | (setup[5] << 8));
    uint16_t wLength  = (uint16_t)(setup[6] | (setup[7] << 8));

    if ((bmReqType & 0x60) == 0x00) {
        /* 标准请求 */
        if (bmReqType == 0x80 && bRequest == 0x06) {   /* GET_DESCRIPTOR */
            uint8_t buf[512];
            int len = 0;
            switch (wValue >> 8) {
            case 0x01:  /* DEVICE */
                len = (int)sizeof(tusb_desc_device_t);
                memcpy(buf, dap_usb_device_desc(), len);
                break;
            case 0x02:  /* CONFIG */
                len = dap_usb_config_desc(buf, sizeof(buf));
                break;
            case 0x03:  /* STRING */
                len = string_to_desc(wValue & 0xFF, buf, sizeof(buf));
                break;
            case 0x22:  /* HID REPORT（按接口） */
                if (wIndex < DAP_PORT_COUNT) {
                    len = dap_usb_hid_report_desc(buf, sizeof(buf));
                }
                break;
            default:
                break;
            }
            if (len > (int)wLength) len = wLength;
            send_ret_submit(c, h, 0, len, len > 0 ? buf : NULL);
            return;
        }
        /* SET_CONFIGURATION / SET_ADDRESS 等：空响应 */
        send_ret_submit(c, h, 0, 0, NULL);
        return;
    }

    if ((bmReqType & 0x60) == 0x20) {
        /* 类请求：SET_REPORT（0x09）= DAP 命令；wIndex = 接口号 = SWD 端口 */
        if (bmReqType == 0x21 && bRequest == 0x09 && wIndex < DAP_PORT_COUNT) {
            int port = wIndex;
            uint8_t rsp[64];
            uint32_t num;
            xSemaphoreTake(dap_port_lock_get(port), portMAX_DELAY);
            num = dap_ports[port]->execute(data_in, rsp);
            xSemaphoreGive(dap_port_lock_get(port));
            uint16_t rsp_len = (uint16_t)(num & 0xFFFFU);

            if (rsp_len > 0) {
                xSemaphoreTake(s_epin_mutex, portMAX_DELAY);
                memcpy(s_epin[port].data, rsp, rsp_len);
                s_epin[port].data_len = rsp_len;
                s_epin[port].data_ready = true;
                xSemaphoreGive(s_epin_mutex);
            }
            send_ret_submit(c, h, 0, 0, NULL);
            epin_flush(port, c);
            return;
        }
        send_ret_submit(c, h, 0, 0, NULL);
        return;
    }

    send_ret_submit(c, h, 0, 0, NULL);
}

/* ── ep1/ep2 中断 IN ── */

static void handle_epin(int c, usbip_stage2_header *h, int port)
{
    xSemaphoreTake(s_epin_mutex, portMAX_DELAY);
    if (s_epin[port].data_ready) {
        uint8_t data[64];
        uint16_t len = s_epin[port].data_len;
        memcpy(data, s_epin[port].data, len);
        s_epin[port].data_ready = false;
        xSemaphoreGive(s_epin_mutex);
        send_ret_submit(c, h, 0, len, data);
    } else if (!s_epin[port].pending) {
        /* 挂起：等 SET_REPORT 产出响应后补回（Windows HID 每端点单 URB） */
        s_epin[port].pending = true;
        s_epin[port].seqnum = h->base.seqnum;
        xSemaphoreGive(s_epin_mutex);
    } else {
        /* 已有挂起（驱动预取）：立即回空 */
        xSemaphoreGive(s_epin_mutex);
        send_ret_submit(c, h, 0, 0, NULL);
    }
}

/* SET_REPORT 产出响应后，唤醒该端口挂起的 IN URB */
static void epin_flush(int port, int c)
{
    xSemaphoreTake(s_epin_mutex, portMAX_DELAY);
    if (s_epin[port].pending && s_epin[port].data_ready) {
        s_epin[port].pending = false;
        uint8_t data[64];
        uint16_t len = s_epin[port].data_len;
        uint32_t seqnum = s_epin[port].seqnum;
        memcpy(data, s_epin[port].data, len);
        s_epin[port].data_ready = false;
        xSemaphoreGive(s_epin_mutex);

        usbip_stage2_header rsp;
        memset(&rsp, 0, sizeof(rsp));
        rsp.base.command   = htonl(USBIP_RET_SUBMIT);
        rsp.base.seqnum    = htonl(seqnum);
        rsp.base.direction = htonl(USBIP_DIR_OUT);   /* 响应方向反转（IN 请求→OUT 响应） */
        rsp.base.ep        = htonl((uint32_t)(port + 1));
        rsp.u.ret_submit.status      = 0;
        rsp.u.ret_submit.data_length = htonl(len);
        send_all(c, &rsp, USBIP_STAGE2_HDR_SIZE);
        send_all(c, data, len);
    } else {
        xSemaphoreGive(s_epin_mutex);
    }
}

/* ── stage2 URB 循环 ── */

static void handle_urbs(int c)
{
    while (1) {
        usbip_stage2_header h;
        if (recv_exact(c, &h, USBIP_STAGE2_HDR_SIZE) < 0) return;

        uint32_t command  = ntohl(h.base.command);
        uint32_t dir      = ntohl(h.base.direction);
        uint32_t ep       = ntohl(h.base.ep);
        int32_t data_len  = ntohl(h.u.cmd_submit.data_length);
        uint8_t data[512];

        if (command == USBIP_CMD_SUBMIT) {
            if (data_len > 0 && dir == USBIP_DIR_OUT) {
                if (data_len > (int)sizeof(data)) return;
                if (recv_exact(c, data, data_len) < 0) return;
            }
            if (ep == 0) {
                handle_control(c, &h, h.u.cmd_submit.setup, data, data_len);
                /* SET_REPORT 可能刚产出响应：补回该端口挂起的 IN URB */
                if ((h.u.cmd_submit.setup[0] & 0x60) == 0x20 &&
                    h.u.cmd_submit.setup[1] == 0x09) {
                    int port = (int)(h.u.cmd_submit.setup[4] | (h.u.cmd_submit.setup[5] << 8));
                    if (port < DAP_PORT_COUNT) epin_flush(port, c);
                }
            } else if (dir == USBIP_DIR_IN && ep == 1) {   /* 单 DAP（SWD1） */
                handle_epin(c, &h, 0);
            } else {
                send_ret_submit(c, &h, 0, 0, NULL);
            }
        } else if (command == USBIP_CMD_UNLINK) {
            usbip_stage2_header rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.base.command  = htonl(USBIP_RET_UNLINK);
            rsp.base.seqnum   = h.base.seqnum;
            rsp.base.direction = htonl(USBIP_DIR_OUT);
            rsp.u.ret_unlink.status = 0;
            send_all(c, &rsp, USBIP_STAGE2_HDR_SIZE);
        } else {
            ESP_LOGW(TAG, "unknown URB command 0x%X", (unsigned)command);
            return;
        }
    }
}

/* ── stage1 握手 ── */

static void send_stage1_header(int c, uint16_t command)
{
    usbip_stage1_header hdr = {
        .version = htons(0x0111),
        .command = htons(command),
        .status  = 0,
    };
    send_all(c, &hdr, sizeof(hdr));
}

static void send_device_info(int c)
{
    usbip_stage1_usb_device dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.path, "/sys/devices/platform/vhci_hcd.0/usb1/1-1");
    strcpy(dev.busid, "1-1");
    dev.busnum = htonl(1);
    dev.devnum = htonl(1);
    dev.speed  = htonl(3);   /* USB_SPEED_FULL */
    dev.idVendor  = htons(0x303A);
    dev.idProduct = htons(0x4008);
    dev.bcdDevice = htons(0x0100);
    dev.bDeviceClass = 0x00;
    dev.bDeviceSubClass = 0x00;
    dev.bDeviceProtocol = 0x00;
    dev.bConfigurationValue = 1;
    dev.bNumConfigurations = 1;
    dev.bNumInterfaces = 1;   /* 单 DAP（SWD1），与 USB 模式一致 */
    send_all(c, &dev, sizeof(dev));
}

static void send_interface_info(int c)
{
    usbip_stage1_usb_interface itf = {
        .bInterfaceClass = 0x03,   /* HID */
        .bInterfaceSubClass = 0x00,
        .bInterfaceProtocol = 0x00,
        .padding = 0,
    };
    for (int i = 0; i < 1; i++) {   /* 单 DAP（SWD1） */
        send_all(c, &itf, sizeof(itf));
    }
}

static int handle_stage1(int c)
{
    uint8_t buf[40];
    if (recv_exact(c, buf, 4) < 0) return -1;
    uint16_t command = (uint16_t)((buf[2] << 8) | buf[3]);

    if (command == (USBIP_OP_REQ_DEVLIST & 0xFF) ||
        command == USBIP_OP_REQ_DEVLIST) {
        /* OP_REQ_DEVLIST：8 字节（4 已收 + 4 status） */
        if (recv_exact(c, buf + 4, 4) < 0) return -1;
        send_stage1_header(c, USBIP_OP_REP_DEVLIST);
        usbip_stage1_devlist dl = { .list_size = htonl(1) };
        send_all(c, &dl, sizeof(dl));
        send_device_info(c);
        send_interface_info(c);
        return 0;
    }
    if (command == (USBIP_OP_REQ_IMPORT & 0xFF) ||
        command == USBIP_OP_REQ_IMPORT) {
        /* OP_REQ_IMPORT：40 字节（4 已收 + 36：status 4 + busid 32） */
        if (recv_exact(c, buf + 4, 36) < 0) return -1;
        send_stage1_header(c, USBIP_OP_REP_IMPORT);
        send_device_info(c);
        /* 注意：不发送 interface 记录——usbip-win 的 query_import_device
         * 只读 sizeof(op_import_reply)=312（device），多发会污染 TCP 流 */
        return 0;
    }
    ESP_LOGW(TAG, "unknown stage1 command 0x%04X", command);
    return -1;
}

/* ── 连接处理 ── */

static void handle_client(int c)
{
    ESP_LOGI(TAG, "client connected");
    if (handle_stage1(c) < 0) return;
    handle_urbs(c);
    ESP_LOGI(TAG, "client disconnected");
}

/* ── server 任务 ── */

static void usbip_server_task(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(USBIP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d failed", USBIP_PORT);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    listen(srv, 2);
    s_sock = srv;
    ESP_LOGI(TAG, "USB/IP server listening on port %d", USBIP_PORT);

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        if (select(srv + 1, &rfds, NULL, NULL, &tv) < 0) continue;
        if (!FD_ISSET(srv, &rfds)) continue;

        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int c = accept(srv, (struct sockaddr *)&ca, &cl);
        if (c < 0) continue;

        /* 新连接前清空各端口挂起状态（旧会话残留） */
        xSemaphoreTake(s_epin_mutex, portMAX_DELAY);
        memset(s_epin, 0, sizeof(s_epin));
        xSemaphoreGive(s_epin_mutex);

        handle_client(c);
        close(c);
    }

    close(srv);
    s_sock = -1;
    vTaskDelete(NULL);
}

esp_err_t usbip_server_start(void)
{
    if (s_running) return ESP_OK;
    s_running = true;
    /* epin 槽锁懒创建 + 永久保留（崩溃根因：此前从未初始化） */
    if (!s_epin_mutex) {
        s_epin_mutex = xSemaphoreCreateMutex();
        if (!s_epin_mutex) {
            s_running = false;
            return ESP_ERR_NO_MEM;
        }
    }
    /* 任务栈放 PSRAM：内部 RAM 在 WiFi+USB DAP 同时开启时紧张；
     * 本任务只做 TCP 收发 + DAP 执行（无 WiFi API），PSRAM 栈安全 */
    if (xTaskCreateWithCaps(usbip_server_task, "usbip_server", 4096, NULL, 5,
                            &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t usbip_server_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    if (s_sock >= 0) shutdown(s_sock, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    s_task = NULL;
    return ESP_OK;
}

bool usbip_server_is_running(void)
{
    return s_running;
}
