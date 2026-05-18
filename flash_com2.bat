@echo off
echo ========================================
echo ESP32-S3 Firmware Flash Script
echo ========================================
echo.

set PYTHON=C:\Espressif\python_env\idf6.0.1_py3.13_env\Scripts\python.exe
set ESPTOOL=%PYTHON% -m esptool

echo Step 1: Erasing flash...
%ESPTOOL% --chip esp32s3 --port COM2 erase-flash
if errorlevel 1 (
    echo [ERROR] Erase failed!
    goto :end
)

echo.
echo Step 2: Flashing bootloader at 0x1000...
%ESPTOOL% --chip esp32s3 --port COM2 write-flash 0x1000 build\bootloader\bootloader.bin
if errorlevel 1 (
    echo [ERROR] Bootloader flash failed!
    goto :end
)

echo.
echo Step 3: Flashing partition table at 0x8000...
%ESPTOOL% --chip esp32s3 --port COM2 write-flash 0x8000 build\partition_table\partition-table.bin
if errorlevel 1 (
    echo [ERROR] Partition table flash failed!
    goto :end
)

echo.
echo Step 4: Flashing application at 0x10000...
%ESPTOOL% --chip esp32s3 --port COM2 write-flash 0x10000 build\wifi_esp32.bin
if errorlevel 1 (
    echo [ERROR] Application flash failed!
    goto :end
)

echo.
echo ========================================
echo SUCCESS! Flash completed.
echo ========================================
:end
