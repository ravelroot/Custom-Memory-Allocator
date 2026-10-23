#!/usr/bin/env sh
set -eu
lib="$1"
export LD_PRELOAD="$lib"
out=$(ls / | sort | head -n 3)
test -n "$out"
echo "favo" | grep -q favo
awk 'BEGIN { s = ""; for (i = 0; i < 20000; i++) s = s "x"; print length(s) }' | grep -q 20000
if command -v python3 >/dev/null 2>&1; then
  python3 -c "d = {i: str(i) * 10 for i in range(100000)}; assert len(d) == 100000"
fi
FAVO_POLICY=best FAVO_STATS=1 bash -c 'x=$(seq 1 2000); test -n "$x"' > /dev/null 2> stats.txt || true
grep -q "politica=best-fit" stats.txt
rm -f stats.txt
echo "preload ok"
