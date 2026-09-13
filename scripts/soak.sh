#!/system/bin/sh
# Passive Android/KernelSU observer. No service mutation or network probes.
# Run as root: sh soak.sh start [seconds] (default 86400, max 604800).
# stop requests a stop at the next 60-second wakeup; it sends no signals.
# ATP_ROOT may point to an isolated test layout. SOAK_BUSYBOX may select BusyBox.
# Existing results are retained: move run/soak elsewhere before another start.

umask 077
LC_ALL=C
export LC_ALL
ROOT=${ATP_ROOT:-/data/adb/atp}
OUT=$ROOT/run/soak
SCRIPT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)/$(basename "$0")
ANALYZER=${SCRIPT%/*}/soak.awk
BB=${SOAK_BUSYBOX:-}
if [ -z "$BB" ]; then
    for candidate in /data/adb/ksu/bin/busybox /data/adb/magisk/busybox /data/adb/ap/bin/busybox; do
        if [ -x "$candidate" ]; then BB=$candidate; break; fi
    done
fi
tool() { if [ -n "$BB" ]; then "$BB" "$@"; else "$@"; fi; }
die() { printf '%s\n' "$*" >&2; exit 1; }
uptime_s() { read -r up rest < /proc/uptime; printf '%s\n' "${up%%.*}"; }
starttime() {
    tool awk '{sub(/^.*\) /, ""); print $20}' "/proc/$1/stat" 2>/dev/null
}
running() {
    [ -r "$OUT/owner" ] || return 1
    read -r owner birth boot < "$OUT/owner"
    read -r current_boot < /proc/sys/kernel/random/boot_id
    [ "$boot" = "$current_boot" ] || return 1
    case "$owner:$birth" in *[!0-9:]*|:|*:|:*) return 1;; esac
    [ "$(starttime "$owner")" = "$birth" ] && [ ! -f "$OUT/finished" ]
}
analyze() {
    now=$(uptime_s)
    tool awk -v out="$OUT" -v now="$now" -v emit="${1:-0}" \
        -v hz="$HZ" -f "$ANALYZER" "$OUT/metrics.csv" > "$OUT/summary.tmp" &&
        tool mv "$OUT/summary.tmp" "$OUT/summary.txt"
}
# A missing PID file is unknown, not proof of process disappearance.
# Re-read starttime after /proc reads to reject exits and PID reuse mid-sample.
process() {
    previous=${2:-NA}
    pid=NA
    [ ! -r "$ROOT/run/$1.pid" ] || read -r pid < "$ROOT/run/$1.pid"
    case "$pid" in ''|*[!0-9]*|0|1)
        pid=${previous%%,*}; remainder=${previous#*,}; previous_birth=${remainder%%,*}
        case "$pid:$previous_birth" in *[!0-9:]*|:|*:|:*) printf 'NA,NA,UNKNOWN,NA,NA,NA,NA'; return;; esac
        if [ ! -d "/proc/$pid" ]; then printf '%s,%s,0,NA,NA,NA,NA' "$pid" "$previous_birth"; return; fi
        observed_birth=$(starttime "$pid")
        if [ -n "$observed_birth" ] && [ "$observed_birth" != "$previous_birth" ]; then
            printf '%s,%s,0,NA,NA,NA,NA' "$pid" "$previous_birth"; return
        fi
        ;;
    esac
    if [ ! -d "/proc/$pid" ]; then printf '%s,NA,0,NA,NA,NA,NA' "$pid"; return; fi
    name=
    read -r name < "/proc/$pid/comm" 2>/dev/null
    if [ "$name" != "$1" ]; then printf '%s,NA,UNKNOWN,NA,NA,NA,NA' "$pid"; return; fi
    stat=$(tool awk '{sub(/^.*\) /, ""); print $20, $12+$13, $1}' "/proc/$pid/stat" 2>/dev/null)
    set -- $stat
    birth=${1:-NA}; ticks=${2:-NA}; state=${3:-NA}
    resources=$(tool awk '/^VmRSS:/ {r=$2} /^Threads:/ {t=$2} END {print r==""?"NA":r, t==""?"NA":t}' "/proc/$pid/status" 2>/dev/null)
    set -- $resources
    rss=${1:-NA}; threads=${2:-NA}; fds=NA
    if [ -r "/proc/$pid/fd" ]; then
        fds=0
        for fd in /proc/"$pid"/fd/*; do
            [ ! -L "$fd" ] || fds=$((fds + 1))
        done
    fi
    if [ "$birth" = NA ] || [ "$(starttime "$pid")" != "$birth" ]; then
        printf '%s,NA,UNKNOWN,NA,NA,NA,NA' "$pid"
    elif [ "$state" = Z ] || [ "$state" = X ]; then
        printf '%s,%s,0,NA,NA,NA,%s' "$pid" "$birth" "$ticks"
    else
        printf '%s,%s,1,%s,%s,%s,%s' "$pid" "$birth" "$rss" "$fds" "$threads" "$ticks"
    fi
}
sample() {
    stamp=$(tool date -u '+%Y-%m-%dT%H:%M:%SZ')
    elapsed=$(uptime_s)
    a=$(process atpd "${a:-NA}")
    b=$(process sing-box "${b:-NA}")
    # Timeout targets only this read-only status client, never the daemon.
    tool timeout -s KILL 5 "$ROOT/atpd" -c "$ROOT/atp.conf" -n status > "$OUT/status.tmp" 2>/dev/null
    rc=$?
    runtime=$(tool awk -v rc="$rc" '
        BEGIN {a=b=h=n=v=i="UNKNOWN"}
        /ATPD DAEMON/ {section="a"; next}
        /PROXY CORE/ {section="b"; next}
        /NATIVE API & MODE/ {section="api"; next}
        /VPN TUNNEL STATUS/ {section="vpn"; next}
        /SYSTEM/ {section="system"; next}
        {sub(/^[^A-Za-z0-9]*/, ""); gsub(/:/," ")}
        section=="a" && $1=="State" {a=$2}
        section=="b" && $1=="STATUS" {b=$2; if ($NF=="healthy" || $NF=="unhealthy") h=$NF}
        section=="b" && $1=="Goroutines" {if ($2=="N/A") n="INVALID"; else if ($2 ~ /^[0-9]+$/) n="VALID"}
        section=="vpn" && /STANDALONE/ {v="NOT_READY"; i="NONE"}
        section=="vpn" && /CONNECTED/ {v="READY"}
        section=="vpn" && $1=="Interface" {i=$2; if (i !~ /^[A-Za-z0-9_.:-]+$/) i="UNKNOWN"}
        END {if (rc!=0) a=b=h=n=v=i="UNKNOWN"; print a "," b "," h "," n "," v "," i}
    ' "$OUT/status.tmp")
    printf '%s,%s,%s,%s,%s,%s\n' "$stamp" "$elapsed" "$a" "$b" "$runtime" "$rc" >> "$OUT/metrics.csv" || return 1
    samples=$((samples + 1))
    analyze "$samples" || return 1
}
HZ=$(getconf CLK_TCK 2>/dev/null) || HZ=0
case "$HZ" in ''|*[!0-9]*) HZ=0;; esac
case "${1:-}" in
    start)
        [ "$(tool id -u)" = 0 ] || die 'Run as root.'
        duration=${2:-86400}
        case "$duration" in ''|*[!0-9]*|????????*) die 'Duration must be 120..604800 seconds.';; esac
        [ "$duration" -ge 120 ] && [ "$duration" -le 604800 ] || die 'Duration must be 120..604800 seconds.'
        [ -x "$ROOT/atpd" ] && [ -r "$ANALYZER" ] || die 'Missing atpd or soak.awk.'
        if running; then die 'Soak already running.'; fi
        tool mkdir -p "$ROOT/run" || exit 1
        tool mkdir "$OUT" 2>/dev/null || die "Results already exist at $OUT; move them before starting a new run."
        printf '%s\n' "$duration" > "$OUT/duration"
        : > "$OUT/events.log"; : > "$OUT/failures.log"
        shell=/system/bin/sh
        [ -x "$shell" ] || shell=/bin/sh
        tool nohup "$shell" "$SCRIPT" _run </dev/null > "$OUT/harness.log" 2>&1 &
        printf 'Started passive soak: %s (%ss); check soak.sh status.\n' "$OUT" "$duration"
        ;;
    _run)
        [ -d "$OUT" ] && [ -f "$OUT/duration" ] || exit 1
        tool mkdir "$OUT/worker.lock" 2>/dev/null || exit 1
        read -r boot < /proc/sys/kernel/random/boot_id
        printf '%s %s %s\n' "$$" "$(starttime "$$")" "$boot" > "$OUT/owner"
        read -r duration < "$OUT/duration"
        started=$(uptime_s)
        printf '%s\n' "$started" > "$OUT/started"
        trap 'printf "interrupted\n" > "$OUT/finished"; exit 1' TERM INT
        printf '%s\n' 'timestamp,uptime_s,atpd_pid,atpd_starttime,atpd_alive,atpd_rss_kb,atpd_fds,atpd_threads,atpd_cpu_ticks,singbox_pid,singbox_starttime,singbox_alive,singbox_rss_kb,singbox_fds,singbox_threads,singbox_cpu_ticks,runtime_state,singbox_state,singbox_health,native_api,vpn_state,vpn_iface,status_exit' > "$OUT/metrics.csv"
        samples=0; reason=duration_complete
        while :; do
            if [ -f "$OUT/stop.request" ]; then reason=stopped; break; fi
            sample || { reason=write_failure; break; }
            # Hard bounds preserve evidence and stop ONLY the observer on overflow.
            bytes=$(tool wc -c < "$OUT/metrics.csv")
            event_bytes=$(tool wc -c < "$OUT/events.log")
            failure_bytes=$(tool wc -c < "$OUT/failures.log")
            if [ "$bytes" -ge 8388608 ] || [ "$event_bytes" -ge 2097152 ] || [ "$failure_bytes" -ge 2097152 ]; then reason=size_limit; break; fi
            now=$(uptime_s)
            [ $((now - started)) -lt "$duration" ] || break
            # Suspend delays samples; never hold a wakelock or replay missed samples.
            delay=$((60 - (now - elapsed)))
            [ "$delay" -gt 0 ] || delay=60
            tool sleep "$delay"
        done
        printf '%s\n' "$reason" > "$OUT/finished"
        analyze 0
        ;;
    stop)
        if running; then : > "$OUT/stop.request"; printf 'Stop requested; completes within 60s plus one status timeout (unless suspended).\n'
        else printf 'Soak is not running.\n'; fi
        ;;
    status)
        if running; then printf 'RUNNING'; [ ! -f "$OUT/stop.request" ] || printf ' (stop requested)'; printf '\n'
        else printf 'NOT RUNNING\n'; fi
        [ ! -f "$OUT/finished" ] || tool cat "$OUT/finished"
        [ ! -f "$OUT/summary.txt" ] || tool cat "$OUT/summary.txt"
        ;;
    summary)
        [ -f "$OUT/summary.txt" ] || die 'No completed sample yet.'
        tool cat "$OUT/summary.txt"
        ;;
    *) die 'Usage: soak.sh {start [seconds]|status|stop|summary}';;
esac
