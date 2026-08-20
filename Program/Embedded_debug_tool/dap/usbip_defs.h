#ifndef USBIP_DEFS_H
#define USBIP_DEFS_H

/* USB/IP 协议定义（设备端 server 用）。
 * 参考 windowsair/wireless-esp8266-dap（MIT）的 usbip_defs.h 精简；
 * 协议规范：kernel.org USB/IP 文档 + usbip-win 兼容格式。 */

#include <stdint.h>

#define USBIP_PORT        872   /* 标准 USB/IP 端口 */
#define USBIP_SYSFS_SIZE  256
#define USBIP_BUSID_SIZE  32

/* stage1 命令（响应 command 为请求值去掉 0x8000 高位） */
enum {
    USBIP_OP_REQ_DEVLIST = 0x8005,
    USBIP_OP_REQ_IMPORT  = 0x8003,
    USBIP_OP_REP_DEVLIST = 0x0005,
    USBIP_OP_REP_IMPORT  = 0x0003,
};

/* stage2 命令（URB） */
enum {
    USBIP_CMD_SUBMIT = 0x0001,
    USBIP_CMD_UNLINK = 0x0002,
    USBIP_RET_SUBMIT = 0x0003,
    USBIP_RET_UNLINK = 0x0004,
};

enum {
    USBIP_DIR_OUT = 0,
    USBIP_DIR_IN  = 1,
};

typedef struct __attribute__((packed)) {
    uint16_t version;   /* 0x0111 (273) */
    uint16_t command;
    uint32_t status;
} usbip_stage1_header;

typedef struct __attribute__((packed)) {
    char path[USBIP_SYSFS_SIZE];
    char busid[USBIP_BUSID_SIZE];
    uint32_t busnum;
    uint32_t devnum;
    uint32_t speed;          /* USB_SPEED_FULL = 3 */
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bConfigurationValue;
    uint8_t  bNumConfigurations;
    uint8_t  bNumInterfaces;
} usbip_stage1_usb_device;

typedef struct __attribute__((packed)) {
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t padding;
} usbip_stage1_usb_interface;

typedef struct __attribute__((packed)) {
    uint32_t list_size;
} usbip_stage1_devlist;

typedef struct __attribute__((packed)) {
    uint32_t command;
    uint32_t seqnum;
    uint32_t devid;
    uint32_t direction;
    uint32_t ep;
} usbip_stage2_basic;

typedef struct __attribute__((packed)) {
    uint32_t transfer_flags;
    int32_t  data_length;
    int32_t  start_frame;
    int32_t  number_of_packets;
    int32_t  interval;
    uint8_t  setup[8];
} usbip_stage2_cmd_submit;

typedef struct __attribute__((packed)) {
    int32_t status;
    int32_t data_length;
    int32_t start_frame;
    int32_t number_of_packets;
    int32_t error_count;
} usbip_stage2_ret_submit;

typedef struct __attribute__((packed)) {
    uint32_t seqnum;
} usbip_stage2_cmd_unlink;

typedef struct __attribute__((packed)) {
    int32_t status;
    uint8_t padding[24];
} usbip_stage2_ret_unlink;

typedef struct __attribute__((packed)) {
    usbip_stage2_basic base;
    union {
        usbip_stage2_cmd_submit cmd_submit;
        usbip_stage2_ret_submit ret_submit;
        usbip_stage2_cmd_unlink cmd_unlink;
        usbip_stage2_ret_unlink ret_unlink;
    } u;
} usbip_stage2_header;

#define USBIP_STAGE2_HDR_SIZE ((int)sizeof(usbip_stage2_header))

#endif /* USBIP_DEFS_H */
