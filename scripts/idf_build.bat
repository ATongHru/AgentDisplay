@echo off
set MSYSTEM=
set MSYS=
set IDF_PATH=D:\Espressif\.espressif\v5.4.2\esp-idf
set IDF_TOOLS_PATH=D:\Espressif\tools
set IDF_PYTHON_ENV_PATH=D:\Espressif\tools\python\v5.4.2\venv
set IDF_CCACHE_ENABLE=1
set ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011\
set IDF_COMPONENT_LOCAL_STORAGE_URL=file://D:/Espressif/tools
set PATH=D:\Espressif\tools\python\v5.4.2\venv\Scripts;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\ccache\4.10.2\ccache-4.10.2-windows-x86_64;D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;D:\Espressif\tools\idf-exe\1.0.3;%PATH%
cd /d "%~dp0.."
python "%IDF_PATH%\tools\idf.py" %*
