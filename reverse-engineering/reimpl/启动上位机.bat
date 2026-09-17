@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo 正在启动 张大头伺服 上位机 (复现版) ...
python zdt_gui.py
if errorlevel 1 (
  echo.
  echo 启动失败。请确认已安装 Python 3 并加入 PATH。
  echo 也可尝试用: py -3 zdt_gui.py
  pause
)
