#!/bin/sh
# Synthetic checks only; never starts, stops, or signals ATPD/sing-box.
# With --dry-run: observe a read-only status fixture for 120 seconds, at 60s intervals.
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
testdir=$(mktemp -d /tmp/atpd-passive-soak.XXXXXX)
printf 'Test artifacts: %s\n' "$testdir"
mkdir "$testdir/events"
awk 'BEGIN {
    print "header"
    for (i=0;i<=10;i++) print "2026-01-01T00:00:00Z," 100+i*60 ",10,1,1," 10000+i*500 ",10,2,0,20,2,1,20000,10,2,0,RUNNING,RUNNING,healthy,VALID,NOT_READY,NONE,0"
    print "2026-01-01T00:11:00Z,760,11,3,1,9000,10,2,0,20,2,0,NA,NA,NA,NA,RUNNING,FAILED,unhealthy,INVALID,READY,tun0,0"
    print "2026-01-01T00:12:00Z,820,11,3,1,9000,50,12,0,21,4,1,21000,10,2,0,RUNNING,RUNNING,healthy,VALID,READY,tun0,0"
}' > "$testdir/input.csv"
awk -v out="$testdir/events" -v now=820 -v emit=13 -v hz=100 -f "$repo/scripts/soak.awk" "$testdir/input.csv" > "$testdir/summary"
grep -q 'Total samples: 13' "$testdir/summary"
grep -q 'ATPD PID changes: 1' "$testdir/summary"
grep -q 'sing-box PID changes: 1' "$testdir/summary"
grep -q 'ATPD sustained RSS growth warnings: 1' "$testdir/summary"
grep -q 'Health failure samples/episodes: 1/1' "$testdir/summary"
grep -q 'Native API invalid samples/episodes: 1/1' "$testdir/summary"
grep -q 'VPN transitions: 1' "$testdir/summary"
grep -q 'ATPD_fd_growth' "$testdir/events/events.log"
grep -q 'ATPD_thread_growth' "$testdir/events/events.log"
grep -q 'sing-box_unexpected_restart' "$testdir/events/failures.log"
printf 'Event/trend/summary checks passed.\n'
# Exercise /proc collection against THIS test shell, never product processes.
sed '/^HZ=/,$d' "$repo/scripts/soak.sh" > "$testdir/functions.sh"
(
    . "$testdir/functions.sh"
    ROOT=$testdir
    mkdir "$ROOT/run"
    read -r test_name < "/proc/$$/comm"
    printf '%s\n' "$$" > "$ROOT/run/$test_name.pid"
    record=$(process "$test_name")
    printf '%s\n' "$record" | awk -F, 'NF!=7 || $3!=1 || $4!~/^[0-9]+$/ || $5!~/^[0-9]+$/ || $6!~/^[0-9]+$/ || $7!~/^[0-9]+$/ {exit 1}'
    absent=$(process absent "$$,1,1,0,0,0,0")
    [ "$absent" = "$$,1,0,NA,NA,NA,NA" ]
    unknown=$(process absent)
    [ "$unknown" = 'NA,NA,UNKNOWN,NA,NA,NA,NA' ]
)
printf '/proc identity/resource and stale-PID checks passed.\n'
case "${1:-}" in --dry-run|--stop-test) ;; *) exit 0;; esac
[ "$(id -u)" = 0 ] || { echo 'Dry-run start requires root.' >&2; exit 1; }
mkdir "$testdir/root"
cat > "$testdir/root/atpd" <<'FIXTURE'
#!/bin/sh
# Fail any command other than the permitted read-only invocation.
[ "$#" = 4 ] && [ "$1" = -c ] && [ "$3" = -n ] && [ "$4" = status ] || exit 99
printf 'status\n' >> "${0%/*}/calls"
printf '%s\n' '=== ATPD DAEMON ===' 'State STOPPED' '=== PROXY CORE ===' 'STATUS STOPPED' '=== VPN TUNNEL STATUS ===' 'State STANDALONE / DIRECT'
FIXTURE
chmod 700 "$testdir/root/atpd"
export ATP_ROOT="$testdir/root"
sh "$repo/scripts/soak.sh" start 120
sleep 1
sh "$repo/scripts/soak.sh" status
if [ "$1" = --stop-test ]; then
    sh "$repo/scripts/soak.sh" stop
    sleep 65
    grep -q '^stopped$' "$ATP_ROOT/run/soak/finished"
    [ "$(wc -l < "$ATP_ROOT/calls")" -eq 1 ]
    printf 'Active stop passed: observer stopped without another status call or service signals.\n'
    exit 0
fi
# Waits are test-only; observer itself always samples at 60 seconds.
sleep 125
sh "$repo/scripts/soak.sh" summary
grep -q '^duration_complete$' "$ATP_ROOT/run/soak/finished"
grep -q 'Total samples: 3' "$ATP_ROOT/run/soak/summary.txt"
[ "$(wc -l < "$ATP_ROOT/calls")" -eq 3 ]
[ "$(wc -l < "$ATP_ROOT/run/soak/metrics.csv")" -eq 4 ]
sh "$repo/scripts/soak.sh" stop
printf '120-second dry-run passed: 3 read-only calls, 3 samples; no product processes controlled.\n'
