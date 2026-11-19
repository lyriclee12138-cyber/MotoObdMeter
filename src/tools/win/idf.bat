@echo off
REM MotoObdMeter - ESP-IDF 5.5.5 wrapper (Windows / LEE-HOME). ASCII only on purpose:
REM cmd.exe reads this file as GBK, so non-ASCII comments would corrupt parsing.
REM Usage: idf.bat <idf.py args>   e.g.  idf.bat -B build build
setlocal
REM Must clear MSYSTEM: Git Bash exports it, and idf.py then skips main() and exits 0 silently.
set "MSYSTEM="
set "MSYS="
set "IDF_TOOLS_PATH=C:\Espressif\tools"
set "IDF_PATH=C:\esp\v5.5.5\esp-idf"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.5\venv"
set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011"
set "IDF_PYTHON_CHECK_CONSTRAINTS=0"
set "PATH=C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\python\v5.5.5\venv\Scripts;%PATH%"
"C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" %*
endlocal
