@echo off
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"

echo [start] %date% %time% 准备启动 Agent Display 后端
echo [start] 仓库目录: %CD%
echo [start] 监听地址: http://0.0.0.0:8000
echo [start] Dashboard: http://127.0.0.1:8000/

where python >nul 2>&1
if errorlevel 1 (
  echo [start] 失败: 找不到 python，请先安装并加入 PATH
  exit /b 1
)

if not exist "%CD%\backend\main.py" (
  echo [start] 失败: 未找到 backend\main.py
  exit /b 1
)

python "%CD%\backend\_wait_health.py" --once >nul 2>&1
if not errorlevel 1 (
  echo [start] 成功: 后端已在运行，8000 端口健康检查通过，无需重复启动
  echo [start] Dashboard: http://127.0.0.1:8000/
  exit /b 0
)

echo [start] 正在后台拉起 uvicorn（无控制台窗口）...
where pythonw >nul 2>&1
if errorlevel 1 (
  echo [start] 失败: 找不到 pythonw，请用官方安装包安装 Python 并加入 PATH
  exit /b 1
)
start "" pythonw "%CD%\backend\_run_hidden.py"

echo [start] 等待健康检查 GET http://127.0.0.1:8000/health ...
python "%CD%\backend\_wait_health.py"
if errorlevel 1 (
  echo [start] 失败: 等待超时，8000 端口未就绪
  echo [start] 请查看 backend\backend.log
  exit /b 1
)

set "PID="
for /f "tokens=5" %%P in ('netstat -ano ^| findstr ":8000" ^| findstr "LISTENING"') do (
  if not defined PID set "PID=%%P"
)
if defined PID (
  echo %PID%> "%CD%\backend\backend.pid"
  echo [start] 成功: 后端已启动  PID=%PID%
) else (
  echo [start] 成功: 健康检查已通过
)
echo [start] Dashboard: http://127.0.0.1:8000/
echo [start] WebSocket:  ws://127.0.0.1:8000/ws
exit /b 0
