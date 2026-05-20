import os
import sys
import subprocess

# Clear MSYS detection
for key in ['MSYSTEM', 'MSYS', 'MINGW', 'MINGW_PREFIX', 'MINGW_CHOST']:
    if key in os.environ:
        del os.environ[key]

os.environ['IDF_PATH'] = r'C:\ESP-IDF\v6.0.1\esp-idf'
os.environ['IDF_TOOLS_PATH'] = r'C:\Espressif'
os.environ['ESP_IDF_VERSION'] = '6.0.1'

path_prefix = r'C:\Espressif\python_env\idf6.0.1_py3.13_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;'
os.environ['PATH'] = path_prefix + os.environ.get('PATH', '')

os.chdir(r'D:\develop\ESP32\S3\wifi\wifi_esp32')

print("Building project...")
result = subprocess.run(
    [sys.executable, r'C:\ESP-IDF\v6.0.1\esp-idf\tools\idf.py', 'build'],
    capture_output=False,
    text=True
)
print(f"Exit code: {result.returncode}")
sys.exit(result.returncode)
