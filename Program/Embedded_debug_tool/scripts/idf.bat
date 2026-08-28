@echo off
rem ESP-IDF build wrapper for this project
set MSYSTEM=
set IDF_PATH=D:\esp\v5.5.3\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_TARGET=esp32s3
set SYSTEMROOT=C:\Windows
set USERPROFILE=C:\Users\xeno
set HOME=C:\Users\xeno
set PATH=C:\Espressif\tools\python\v5.5.3\venv\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%
cd /d D:\Task\embedded_debugger_tool\Program\Embedded_debug_tool
python D:\esp\v5.5.3\esp-idf\tools\idf.py %*
