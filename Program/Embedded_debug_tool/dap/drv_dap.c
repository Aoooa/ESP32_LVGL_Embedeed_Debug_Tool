#include "drv_dap.h"

/* 端口表：索引 = USB HID 接口序号（tud_hid instance）。
 * 新增端口 = 建 components/dap_portN 组件（见其 CMakeLists 注释）+ 在此追加一行 */
extern const dap_port_t dap_port_0;
extern const dap_port_t dap_port_1;

const dap_port_t *const dap_ports[DAP_PORT_COUNT] = {
    &dap_port_0,
    &dap_port_1,
};

/* 表条目数与 DAP_PORT_COUNT 一致性检查（加端口忘了改宏 → 编译错误） */
_Static_assert(DAP_PORT_COUNT ==
               (int)(sizeof(dap_ports) / sizeof(dap_ports[0])),
               "DAP_PORT_COUNT mismatch with dap_ports table");
