$env:IDF_PATH = 'C:\esp\esp-idf'
# git-bash 继承下来的 MSYS 变量会让 ESP-IDF 6.2 误判为不受支持的 Mingw 环境
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:MINGW_CHOST -ErrorAction SilentlyContinue
Remove-Item Env:MSYSTEM_PREFIX -ErrorAction SilentlyContinue
Remove-Item Env:SHELL -ErrorAction SilentlyContinue
Remove-Item Env:TERM -ErrorAction SilentlyContinue
# components-file.espressif.com（CDN）本网络不可达，改用国内镜像
$env:IDF_COMPONENT_STORAGE_URL = 'https://components-file.espressif.cn'
$env:IDF_COMPONENT_REGISTRY_URL = 'https://components.espressif.cn'
$env:ESP_ROM_ELF_DIR = 'C:\Users\caocheng\.espressif\tools\esp-rom-elfs\20260528'
$T = 'C:\Users\caocheng\.espressif\tools'
$env:PATH = "$T\cmake\4.0.3\bin;$T\ninja\1.12.1;$T\idf-exe\1.0.3;$T\esp-idf-configdep\0.2.3\esp-idf-configdep-0.2.3\bin;$T\xtensa-esp-elf\esp-16.1.0_20260609\xtensa-esp-elf\bin;$T\riscv32-esp-elf\esp-16.1.0_20260609\riscv32-esp-elf\bin;C:\Users\caocheng\.espressif\python_env\idf6.2_py3.12_env\Scripts;C:\esp\esp-idf\tools;$env:PATH"
Set-Location D:\work\nrl-esp32
python scripts/build.py $args[0] build
