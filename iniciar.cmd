@echo off
setlocal
cd /d "%~dp0"
where docker >nul 2>nul
if errorlevel 1 (
  echo O Docker Desktop nao foi encontrado. Instale em https://www.docker.com/products/docker-desktop/ e rode este arquivo de novo.
  pause
  exit /b 1
)
docker info >nul 2>nul
if errorlevel 1 (
  echo Abra o Docker Desktop e espere ele iniciar. Depois rode este arquivo de novo.
  pause
  exit /b 1
)
docker compose up -d --build
if errorlevel 1 (
  pause
  exit /b 1
)
echo.
echo Favo rodando em http://127.0.0.1:8080
start "" http://127.0.0.1:8080
endlocal
