#!/usr/bin/env sh
set -eu
cli="$1"
dir="$2"
for t in "$dir"/*.trace; do
  for p in first next best; do
    "$cli" run "$t" --policy "$p" --check --quiet > /dev/null
  done
done
"$cli" demo > /dev/null
echo "traces ok"
