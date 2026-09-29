#!/bin/zsh
# Sample the release benchmark and open a flame graph.
#   scripts/flamegraph.sh [seconds]      default 12
# Needs: brew install flamegraph. The default bench run finishes in ~1 s, too short to
# sample, so this runs it with --reps 1500 and kills it when the sample window ends.
set -e
cd "$(dirname "$0")/.."
SECS=${1:-12}
P=$(brew --prefix)/bin
OUT=build-release/flame
mkdir -p "$OUT"
[ -x build-release/apps/bench ] || { echo "build release first: cmake --build --preset release"; exit 1; }

./build-release/apps/bench --reps 1500 > /dev/null 2>&1 &
PID=$!
sleep 0.5
/usr/bin/sample "$PID" "$SECS" -mayDie -file "$OUT/bench.sample" > /dev/null
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null || true

awk -f "$P/stackcollapse-sample.awk" "$OUT/bench.sample" > "$OUT/bench.folded"
"$P/flamegraph.pl" --title "darkstar_hft release bench, ${SECS}s sample at 1ms" --width 1500 \
    "$OUT/bench.folded" > "$OUT/flame.svg"

echo "self time by leaf frame:"
awk '{n=$NF; sub(/ [0-9]+$/,""); split($0,a,";"); leaf=a[length(a)]; s[leaf]+=n; t+=n}
     END {for (k in s) printf "%6d  %5.1f%%  %s\n", s[k], 100*s[k]/t, k}' "$OUT/bench.folded" \
     | sort -nr | head -12
echo
echo "wrote $OUT/flame.svg"
open "$OUT/flame.svg"
