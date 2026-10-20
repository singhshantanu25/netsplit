#!/bin/bash
# Benchmark: Wi-Fi alone vs phone alone vs both, same file, RUNS runs each.
# Usage: tests/bench.sh <url> <wifi_if> <phone_if> [runs=3] [workers=4]
# Example: tests/bench.sh https://speedtest.singapore.linode.com/100MB-singapore.bin en0 en5
# Prints a Markdown table and saves it to bench_results.md (in the current folder).
# WARNING: every run downloads the whole file; "phone" and "both" use mobile data.

set -u
URL=${1:?url}
WIFI=${2:?wifi interface, e.g. en0}
PHONE=${3:?phone interface, e.g. en5}
RUNS=${4:-3}
WORKERS=${5:-4}
HALF=$((WORKERS / 2))

NETSPLIT="$(cd "$(dirname "$0")/.." && pwd)/build/netsplit"
FILE=$(basename "${URL%%\?*}")
WORK=$(mktemp -d)   # downloads go here, deleted at the end
trap 'rm -rf "$WORK"' EXIT

run_config() {   # $1 = label, $2 = --interfaces value
    local label=$1 nets=$2 times="" sum=0 i out t
    for i in $(seq 1 "$RUNS"); do
        rm -f "$WORK/$FILE" "$WORK/$FILE.part" "$WORK/$FILE.progress.json"
        out=$(cd "$WORK" && "$NETSPLIT" get "$URL" --interfaces "$nets" 2>&1 | tr '\r' '\n')
        t=$(echo "$out" | awk '/^Time:/ {print $2}')
        if [ -z "$t" ]; then
            echo "  $label run $i FAILED:" >&2
            echo "$out" | grep -i error >&2
            t="nan"
        fi
        echo "  $label run $i: ${t} s" >&2
        times="$times | $t"
        sum=$(awk -v a="$sum" -v b="$t" 'BEGIN { print a + b }')
    done
    local avg size_mb speed
    avg=$(awk -v s="$sum" -v n="$RUNS" 'BEGIN { printf "%.2f", s / n }')
    size_mb=$(cd "$WORK" && "$NETSPLIT" probe "$URL" 2>/dev/null |
              awk '/^Size:/ { printf "%.1f", $2 / 1048576 }')
    speed=$(awk -v m="$size_mb" -v a="$avg" 'BEGIN { if (a > 0) printf "%.2f", m / a; else print "nan" }')
    echo "| $label | $nets$times | $avg | $speed |"
}

{
    echo "Benchmark: $FILE, $RUNS runs each, $WORKERS workers in total ($(date '+%Y-%m-%d'))"
    echo
    printf "| Network | --interfaces"
    for i in $(seq 1 "$RUNS"); do printf " | run %d (s)" "$i"; done
    echo " | avg (s) | avg MB/s |"
    printf "|---|---"
    for i in $(seq 1 "$RUNS"); do printf "|---"; done
    echo "|---|---|"
    run_config "Wi-Fi only" "$WIFI:$WORKERS"
    run_config "Phone only" "$PHONE:$WORKERS"
    run_config "Both" "$WIFI:$HALF,$PHONE:$((WORKERS - HALF))"
} | tee bench_results.md
