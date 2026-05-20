# Clear MSYS/MINGW detection
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MSYS -ErrorAction SilentlyContinue
Remove-Item Env:MINGW -ErrorAction SilentlyContinue

$env:IDF_PATH = 'C:\ESP-IDF\v6.0.1\esp-idf'
$env:IDF_TOOLS_PATH = 'C:\Espressif'
$env:PATH = 'C:\Espressif\python_env\idf6.0.1_py3.13_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;' + $env:PATH

Set-Location 'D:\develop\ESP32\S3\wifi\wifi_esp32'

Write-Host "Starting build..."
python "$env:IDF_PATH\tools\idf.py" build

Write-Host "Exit code: $LASTEXITCODE"
