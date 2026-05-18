@echo off
echo ========================================
echo ESP32-S3 Build Script
echo ========================================
echo.

set IDF_PATH=C:\ESP-IDF\v6.0.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif
set ESP_IDF_VERSION=6.0.1
set PATH=C:\Espressif\python_env\idf6.0.1_py3.13_env\Scripts;%PATH%
set PATH=C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;%PATH%

echo IDF_PATH=%IDF_PATH%
echo ESP_IDF_VERSION=%ESP_IDF_VERSION%
echo.

echo Reconfiguring project...
python "%IDF_PATH%\tools\idf.py" reconfigure

if errorlevel 1 (
    echo.
    echo [ERROR] Reconfigure failed!
    goto :end
)

echo.
echo Building project...
python "%IDF_PATH%\tools\idf.py" build

if errorlevel 1 (
    echo.
    echo [ERROR] Build failed!
    goto :end
)

echo.
echo ========================================
echo SUCCESS! Build completed.
echo ========================================
:end
