# symFiddle5OBD · bash 用的 ESP-IDF 环境（本机 Windows / LEE-HOME）
# 用法：source /c/workspace/sym_fiddle5_obd/env.sh
# 注意：真正的构建请用 ../idf.bat —— MSYS 的路径转换会篡改 cmake 的 -D 参数
export IDF_TOOLS_PATH="C:/Espressif/tools"
export IDF_PATH="C:/esp/v5.5.5/esp-idf"
export IDF_PYTHON_ENV_PATH="C:/Espressif/tools/python/v5.5.5/venv"
export ESP_ROM_ELF_DIR="C:/Espressif/tools/esp-rom-elfs/20241011"
export IDF_PYTHON_CHECK_CONSTRAINTS=0
export PATH="$IDF_TOOLS_PATH/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$IDF_TOOLS_PATH/cmake/3.30.2/bin:$IDF_TOOLS_PATH/ninja/1.12.1:$IDF_TOOLS_PATH/python/v5.5.5/venv/Scripts:$PATH"
