# dap_port1：SWD 端口 1 实例（复制 dap_port0 改造：引脚 + 前缀宏 _p1）
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

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "DAP_ExecuteCommand=DAP_ExecuteCommand_p1"
    "DAP_ProcessCommand=DAP_ProcessCommand_p1"
    "DAP_ProcessVendorCommand=DAP_ProcessVendorCommand_p1"
    "DAP_Setup=DAP_Setup_p1"
    "Delayms=Delayms_p1"
    "SWJ_Sequence=SWJ_Sequence_p1"
    "SWD_Sequence=SWD_Sequence_p1"
    "SWD_Transfer=SWD_Transfer_p1"
    "DAP_Data=DAP_Data_p1"
    "DAP_TransferAbort=DAP_TransferAbort_p1")
