#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
porta="${FAVO_PORT:-8080}"
if [ "${1:-}" = "--docker" ]; then
  docker compose up -d --build
  echo "Favo rodando em http://127.0.0.1:${porta}"
  exit 0
fi
for cmd in cmake cc python3 npm; do
  command -v "$cmd" >/dev/null 2>&1 || { echo "Falta o programa: $cmd (ou rode ./iniciar.sh --docker)"; exit 1; }
done
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
if [ ! -d web/dist ]; then
  (cd web && npm ci --no-audit --no-fund && npm run build)
fi
if [ ! -d .venv ]; then
  python3 -m venv .venv
fi
.venv/bin/pip install -q -r api/requirements.txt
lib="$PWD/build/libfavo.so"
[ "$(uname)" = "Darwin" ] && lib="$PWD/build/libfavo.dylib"
echo "Favo rodando em http://127.0.0.1:${porta}"
cd api
FAVO_LIBRARY="$lib" FAVO_WEB_DIR="$PWD/../web/dist" FAVO_DATABASE="$PWD/../data/favo.db" exec ../.venv/bin/python -m favo_api --port "$porta"
