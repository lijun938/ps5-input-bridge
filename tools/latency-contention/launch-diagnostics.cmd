@echo off
setlocal
set "PS5BRIDGE_TRACE="
set "PS5BRIDGE_XINPUT_TIMING=1"
set "PS5BRIDGE_DIAGNOSTICS=1"
set "PS5BRIDGE_DIAGNOSTICS_DIR="
start "" /wait "%~dp0Ps5InputBridge.exe"
