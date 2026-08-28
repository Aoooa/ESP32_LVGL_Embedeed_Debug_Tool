@echo off
REM USB 副屏驱动安装 - 双击此文件，按提示完成
REM 驱动: xfz1986 USB Graphics Driver (signed by Airspace Intelligent Technology via Sectigo)
REM 来源: https://github.com/chuanjinpangang/win10_idd_xfz1986_usb_graphic_driver_display
echo ============================================
echo  USB 副屏驱动安装 (xfz1986)
echo ============================================
echo.
echo 该驱动让 Windows 把 ESP32-LCD 识别为副屏
echo 安装后会显示 "xfz1986 USB Graphic Device" 在
echo 显示适配器里, 然后桌面设置多一个 240x320 显示器
echo.
echo 如提示 UAC 用户账户控制, 请选择 "是"
echo 安装完成后, 插入 ESP32 USB 数据线即可识别
echo.
pause
start /wait "" "%~dp0xfz1986.exe"
echo.
echo 安装完成. 按任意键退出.
pause >nul
