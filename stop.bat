@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"

echo [stop] %date% %time% 准备关闭 Agent Display 后端

set "PID="
if exist "%CD%\backend\backend.pid" (
  set /p PID=<"%CD%\backend\backend.pid"
)

set "KILLED=0"

if defined PID (
  echo [stop] 按 PID 文件结束进程 %PID%
  taskkill /PID %PID% /T /F >nul 2>&1
  if not errorlevel 1 set "KILLED=1"
)

echo [stop] 按窗口标题结束 ESP32S3-AGENT-BACKEND
taskkill /FI "WINDOWTITLE eq ESP32S3-AGENT-BACKEND*" /T /F >nul 2>&1
if not errorlevel 1 set "KILLED=1"

for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":8000" ^| findstr "LISTENING"') do (
  echo [stop] 按端口 8000 结束 PID=%%P
  taskkill /PID %%P /T /F >nul 2>&1
  if not errorlevel 1 set "KILLED=1"
)

if exist "%CD%\backend\backend.pid" del /f /q "%CD%\backend\backend.pid" >nul 2>&1

if exist "%USERPROFILE%\.cursor\hooks\backend_online.json" (
  del /f /q "%USERPROFILE%\.cursor\hooks\backend_online.json" >nul 2>&1
  echo [stop] 已删除 Hook 在线标记 backend_online.json
)

python "%CD%\backend\_wait_health.py" --once >nul 2>&1
if not errorlevel 1 (
  echo [stop] 失败: 8000 端口仍在监听，后端可能未退出
  exit /b 1
)

if "%KILLED%"=="1" (
  echo [stop] 成功: 后端已关闭，8000 端口已释放
) else (
  echo [stop] 成功: 没有运行中的后端，8000 端口空闲
)
exit /b 0
