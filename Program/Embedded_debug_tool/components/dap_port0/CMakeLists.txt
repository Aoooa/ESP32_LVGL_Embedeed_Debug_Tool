# dap_port0：SWD 端口 0 实例（内核源码共享 dap/arm/，符号加 _p0 前缀隔离；
# 引脚在 DAP_config.h 定义）。新增端口 = 复制本组件目录，改前缀宏与引脚。
set(ARM_DIR ${CMAKE_CURRENT_LIST_DIR}/../../dap/arm)

idf_component_register(
    SRCS
        "${ARM_DIR}/DAP.c"
        "${ARM_DIR}/SW_DP.c"
        "${ARM_DIR}/DAP_vendor.c"
        "dap_port_instance.c"
    INCLUDE_DIRS
        "."
        "${ARM_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/../../dap"
    PRIV_REQUIRES
        hal soc
)

# 内核全局符号加端口前缀，避免多实例链接冲突
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "DAP_ExecuteCommand=DAP_ExecuteCommand_p0"
    "DAP_ProcessCommand=DAP_ProcessCommand_p0"
    "DAP_ProcessVendorCommand=DAP_ProcessVendorCommand_p0"
    "DAP_Setup=DAP_Setup_p0"
    "Delayms=Delayms_p0"
    "SWJ_Sequence=SWJ_Sequence_p0"
    "SWD_Sequence=SWD_Sequence_p0"
    "SWD_Transfer=SWD_Transfer_p0"
    "DAP_Data=DAP_Data_p0"
    "DAP_TransferAbort=DAP_TransferAbort_p0")
