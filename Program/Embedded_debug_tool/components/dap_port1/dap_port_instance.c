/* dap_port_instance.c —— SWD 端口 1 实例（复制端口 0，符号前缀 _p1，引脚 14/15/18） */

#include "drv_dap.h"
#include "DAP_config.h"
#include "DAP.h"

static void port_init(void)
{
    DAP_Setup();
}

static uint32_t port_execute(const uint8_t *req, uint8_t *rsp)
{
    return DAP_ExecuteCommand(req, rsp);
}

const dap_port_t dap_port_1 = {
    .name = "SWD2 (SWDIO=14 SWCLK=15 RST=18)",
    .init = port_init,
    .execute = port_execute,
};
