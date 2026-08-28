#!/bin/bash
# ESP-IDF wrapper
cd "/d/Task/embedded_debugger_tool/Program/Embedded_debug_tool"
exec env -u MSYSTEM \
    PATH="/c/Espressif/tools/python/v5.5.3/venv/Scripts:/c/Espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin:/c/Espressif/tools/riscv32-esp-elf/esp-14.2.0_20251107/riscv32-esp-elf/bin:/c/Espressif/tools/cmake/3.30.2/bin:/c/Espressif/tools/ninja/1.12.1:/usr/bin:/c/Windows/System32:$PATH" \
    IDF_PATH="/d/esp/v5.5.3/esp-idf" \
    IDF_TOOLS_PATH="/c/Espressif/tools" \
    IDF_TARGET="esp32s3" \
    HOME="/c/Users/xeno" \
    SYSTEMROOT="C:\\Windows" \
    USERPROFILE="C:\\Users\\xeno" \
    python "D:/esp/v5.5.3/esp-idf/tools/idf.py" "$@"
