@echo off
rem SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
setlocal
if "%~1"=="" goto build
if /i "%~1"=="--help" goto help
if /i "%~1"=="--web" goto web
if /i not "%~1"=="--package" goto invalid
if not "%~2"=="" goto invalid
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdk\Build.ps1" -Package
exit /b %errorlevel%
:build
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdk\Build.ps1"
exit /b %errorlevel%
:invalid
echo Use build.bat --web, build.bat or build.bat --package.
exit /b 2
:web
if not "%~2"=="" goto invalid
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Build-Web.ps1"
exit /b %errorlevel%
:help
echo build.bat             Build firmware
echo build.bat --web       Build browser preview
echo build.bat --package   Build firmware and complete workbench
echo Toolchain and output paths: README.md
exit /b 0
