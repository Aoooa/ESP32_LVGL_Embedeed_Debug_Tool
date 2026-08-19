/* dap_port_instance.c —— SWD 端口 0 实例：把内核实例化为 dap_port_t 端口。
 * 注意：本组件所有 TU 的 DAP_ExecuteCommand/DAP_Setup 等符号已被 CMake
 * 前缀宏重命名（_p0），此处直接调用即端口 0 内核。 */

#include "drv_dap.h"
#include "DAP_config.h"
#include "DAP.h"

static void port_init(void)
{
    DAP_Setup();   /* 引脚初始化（DAP_SETUP→dap_pins_init）+ 内核默认状态 */
}

static uint32_t port_execute(const uint8_t *req, uint8_t *rsp)
{
    /* 返回 DAP 内核 num：低 16 位 = 响应长度，高 16 位 = 请求长度 */
    return DAP_ExecuteCommand(req, rsp);
}

const dap_port_t dap_port_0 = {
    .name = "SWD1 (SWDIO=11 SWCLK=12 RST=13)",
    .init = port_init,
    .execute = port_execute,
};
