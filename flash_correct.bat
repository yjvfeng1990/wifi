@echo off
echo Flashing ESP32-S3 with correct addresses...

set IDF_PATH=C:\ESP-IDF\v6.0.1
set PYTHON=C:\Espressif\python_env\idf6.0.1_py3.13_env\Scripts\python.exe
set ESPTOOL=%PYTHON% -m esptool

echo Step 1: Erasing flash...
%ESPTOOL% --chip esp32s3 erase_flash

echo Step 2: Flashing bootloader at 0x1000...
%ESPTOOL% --chip esp32s3 write_flash 0x1000 build\bootloader\bootloader.bin

echo Step 3: Flashing partition table at 0x8000...
%ESPTOOL% --chip esp32s3 write_flash 0x8000 build\partition_table\partition-table.bin

echo Step 4: Flashing application at 0x10000...
%ESPTOOL% --chip esp32s3 write_flash 0x10000 build\wifi_esp32.bin

echo Done!
