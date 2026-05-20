@echo off
setlocal

set IDF_PATH=C:\ESP-IDF\v6.0.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif
set ESP_IDF_VERSION=6.0.1
set PATH=C:\Espressif\python_env\idf6.0.1_py3.13_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;%PATH%

cd /d D:\develop\ESP32\S3\wifi\wifi_esp32

echo Starting build...
python "%IDF_PATH%\tools\idf.py" build

echo Exit code: %ERRORLEVEL%
endlocal
