#!/bin/sh
set -eu

ATPD_BIN=${ATPD_BIN:-build/bin/atpd}
[ -x "$ATPD_BIN" ] || { echo "ATPD binary not found at $ATPD_BIN" >&2; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "C compiler required" >&2; exit 1; }

root=$(mktemp -d)
foreign_pid=
cleanup() {
    if [ -n "${foreign_pid:-}" ]; then
        kill "$foreign_pid" >/dev/null 2>&1 || true
        wait "$foreign_pid" 2>/dev/null || true
    fi
    "$ATPD_BIN" -c "$root/atp.conf" stop >/dev/null 2>&1 || true
    rm -rf "$root"
}
trap cleanup EXIT INT TERM
mkdir -p "$root/bin" "$root/run"

cat > "$root/mock_singbox.c" <<'EOF'
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int serve_native_api(int fd) {
    static const unsigned char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/grpc-web+proto\r\n"
        "Content-Length: 7\r\n\r\n"
        "\x00\x00\x00\x00\x02\x10\x01";
    char request[2048];
    size_t used = 0;

    while (used < sizeof(request) - 1) {
        ssize_t size = recv(fd, request + used, sizeof(request) - 1 - used, 0);
        if (size <= 0) return -1;
        used += (size_t)size;
        request[used] = '\0';
        char *header_end = strstr(request, "\r\n\r\n");
        if (header_end) {
            if (!strstr(request, "POST /daemon.StartedService/SubscribeStatus ")) return -1;
            if (used >= (size_t)(header_end - request) + 4 + 11) break;
        }
    }

    size_t sent = 0;
    while (sent < sizeof(response) - 1) {
        ssize_t size = send(fd, response + sent, sizeof(response) - 1 - sent, 0);
        if (size <= 0) return -1;
        sent += (size_t)size;
    }
    return 0;
}

static void log_command(int argc, char **argv) {
    const char *path = getenv("MOCK_LOG");
    if (!path) return;
    FILE *file = fopen(path, "a");
    if (!file) return;
    for (int i = 1; i < argc; i++) fprintf(file, "%s%s", i == 1 ? "" : " ", argv[i]);
    fputc('\n', file);
    fclose(file);
}

int main(int argc, char **argv) {
    log_command(argc, argv);
    if (argc < 2) return 1;
    if (!strcmp(argv[1], "version")) {
        puts("sing-box version test with_ebpf");
        return 0;
    }
    const char *fail_config = getenv("MOCK_FAIL_CONFIG");
    if (!strcmp(argv[1], "check")) return fail_config && *fail_config ? 1 : 0;
    if (!strcmp(argv[1], "tools")) return 64;
    if (!strcmp(argv[1], "run")) {
        sigset_t inherited_mask;
        if (sigprocmask(SIG_SETMASK, NULL, &inherited_mask) != 0 ||
            sigismember(&inherited_mask, SIGTERM) == 1 ||
            sigismember(&inherited_mask, SIGHUP) == 1 ||
            sigismember(&inherited_mask, SIGCHLD) == 1) {
            fputs("mock sing-box inherited a blocked service signal\n", stderr);
            return 73;
        }
        const char *exit_during_ready = getenv("MOCK_EXIT_DURING_READY");
        if (exit_during_ready && *exit_during_ready) return 42;
        const char *fail_ready = getenv("MOCK_FAIL_READY");
        if (fail_ready && *fail_ready) {
            pause();
            return 0;
        }
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        const char *port_str = getenv("MOCK_API_PORT");
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons((unsigned short)(port_str ? atoi(port_str) : 19080)),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
        };
        if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 8)) return 1;
        for (;;) {
            int client = accept(fd, NULL, NULL);
            if (client >= 0) {
                serve_native_api(client);
                close(client);
            }
        }
    }
    return 0;
}
EOF
cc -O2 -o "$root/bin/sing-box" "$root/mock_singbox.c"

cat > "$root/foreign_listener.c" <<'EOF'
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((unsigned short)atoi(argv[1])),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 8)) return 1;
    FILE *ready = fopen(argv[2], "w");
    if (!ready) return 1;
    fputs("ready\n", ready);
    fclose(ready);
    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client >= 0) close(client);
    }
}
EOF
cc -O2 -o "$root/foreign-listener" "$root/foreign_listener.c"

# Let lifecycle checks run in unprivileged CI containers that cannot subscribe
# to XFRM multicast groups; production behavior is unchanged.
cat > "$root/netlink_shim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/netlink.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*real_bind)(int, const struct sockaddr *, socklen_t);
    if (!real_bind) real_bind = dlsym(RTLD_NEXT, "bind");
    int result = real_bind(fd, addr, len);
    if (result < 0 && errno == EPERM && addr && addr->sa_family == AF_NETLINK) return 0;
    return result;
}

int kill(pid_t pid, int sig) {
    static int (*real_kill)(pid_t, int);
    static pid_t terminating;
    if (!real_kill) real_kill = dlsym(RTLD_NEXT, "kill");
    if (sig == SIGTERM || sig == SIGKILL) terminating = pid;
    int result = real_kill(pid, sig);
    if (result == 0 && sig == 0 && pid == terminating) {
        char path[64], state = 0;
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *file = fopen(path, "r");
        if (file) {
            fscanf(file, "%*d %*s %c", &state);
            fclose(file);
        }
        if (state == 'Z') { errno = ESRCH; return -1; }
    }
    return result;
}
EOF
cc -shared -fPIC -o "$root/netlink_shim.so" "$root/netlink_shim.c" -ldl
sanitizer_runtime=$(ldd "$ATPD_BIN" 2>/dev/null | awk '/lib(asan|tsan)\.so/ { print $3; exit }')
preload=${sanitizer_runtime:+$sanitizer_runtime:}$root/netlink_shim.so

cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
SERVICE_START_TIMEOUT=2
EOF
printf '%s\n' '{"inbounds":[]}' > "$root/config.json"
run_atp() {
    env LD_PRELOAD="$preload" MOCK_LOG="$root/commands" MOCK_API_PORT=19080 \
        MOCK_FAIL_CONFIG="${MOCK_FAIL_CONFIG:-}" MOCK_FAIL_READY="${MOCK_FAIL_READY:-}" \
        MOCK_EXIT_DURING_READY="${MOCK_EXIT_DURING_READY:-}" \
        "$ATPD_BIN" -c "$root/atp.conf" "$@"
}

read_atpd_pid() {
    sed -n '1p' "$1"
}

proc_starttime() {
    stat_line=$(cat "/proc/$1/stat")
    stat_fields=${stat_line##*) }
    set -- $stat_fields
    shift 19
    printf '%s\n' "$1"
}

printf '%s\n' '=== offline status needs no service owner ==='
"$ATPD_BIN" -c "$root/atp.conf" status >"$root/offline-status.out"
grep -q 'STATUS.*STOPPED' "$root/offline-status.out"
[ ! -e "$root/run/atpd.pid" ]
[ ! -e "$root/run/sing-box.pid" ]
[ ! -e "$root/commands" ]

printf '%s\n' '=== config check failure blocks startup ==='
if MOCK_FAIL_CONFIG=1 run_atp start >"$root/config-fail.out" 2>"$root/config-fail.err"; then
    echo "start unexpectedly succeeded" >&2; exit 1
fi
grep -q "sing-box configuration check: FAIL" "$root/config-fail.out"
! grep -q '^run ' "$root/commands"
[ ! -e "$root/run/atpd.pid" ]

printf '%s\n' '=== config pass starts without eBPF capability probe ==='
: > "$root/commands"
run_atp start >"$root/start.out" 2>"$root/start.err"
grep -q "sing-box configuration check: PASS" "$root/start.out"
! grep -q '^tools ' "$root/commands"
grep -Eq 'Daemon started successfully \(PID: [0-9]+\)' "$root/start.out"
pid=$(read_atpd_pid "$root/run/atpd.pid")
[ "$(sed -n '2p' "$root/run/atpd.pid")" = "$(proc_starttime "$pid")" ]
grep -q "Daemon started successfully (PID: $pid)" "$root/start.out"
grep -q "Runtime status:" "$root/start.out"
grep -q "ATPD:      RUNNING (PID: $pid)" "$root/start.out"
grep -q "sing-box:  RUNNING / healthy (PID:" "$root/start.out"
grep -q "Kernel:" "$root/start.out"
grep -q "Data path: sing-box ebpf inbound" "$root/start.out"
grep -q "uptime" "$root/start.out"
grep -q "RSS" "$root/start.out"
kill -0 "$pid"
run_atp stop >/dev/null

printf '%s\n' '=== readiness failure is reported ==='
: > "$root/commands"
if MOCK_FAIL_READY=1 run_atp start >"$root/ready-fail.out" 2>"$root/ready-fail.err"; then
    echo "start unexpectedly succeeded without readiness" >&2; exit 1
fi
! grep -q "Daemon started successfully" "$root/ready-fail.out"
grep -q "Daemon failed to start" "$root/ready-fail.err"
run_atp stop >/dev/null 2>&1 || true

printf '%s\n' '=== foreign TCP listener cannot satisfy Native API readiness ==='
cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19081
SERVICE_START_TIMEOUT=2
EOF
"$root/foreign-listener" 19081 "$root/foreign.ready" &
foreign_pid=$!
while [ ! -s "$root/foreign.ready" ]; do
    kill -0 "$foreign_pid"
done
if MOCK_FAIL_READY=1 run_atp start >"$root/foreign-fail.out" 2>"$root/foreign-fail.err"; then
    echo "foreign listener falsely satisfied readiness" >&2; exit 1
fi
kill -0 "$foreign_pid"
kill "$foreign_pid"
wait "$foreign_pid" 2>/dev/null || true
foreign_pid=
! grep -q "Daemon started successfully" "$root/foreign-fail.out"
grep -q "Daemon failed to start" "$root/foreign-fail.err"
run_atp stop >/dev/null 2>&1 || true
cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
SERVICE_START_TIMEOUT=2
EOF

printf '%s\n' '=== child exit during readiness fails startup ==='
if MOCK_EXIT_DURING_READY=1 run_atp start >"$root/child-exit.out" 2>"$root/child-exit.err"; then
    echo "exited child falsely satisfied readiness" >&2; exit 1
fi
! grep -q "Daemon started successfully" "$root/child-exit.out"
grep -q "Daemon failed to start" "$root/child-exit.err"
run_atp stop >/dev/null 2>&1 || true

printf '%s\n' '=== restart stops before a failing config check ==='
if ! run_atp start >"$root/before-failed-restart.out" 2>"$root/before-failed-restart.err"; then
    cat "$root/before-failed-restart.out" "$root/before-failed-restart.err" >&2
    [ ! -r "$root/run/atp.log" ] || cat "$root/run/atp.log" >&2
    [ ! -r "$root/sing-box.log" ] || cat "$root/sing-box.log" >&2
    echo "setup start for failed restart test did not become ready" >&2
    exit 1
fi
if MOCK_FAIL_CONFIG=1 run_atp restart >"$root/failed-restart.out" 2>"$root/failed-restart.err"; then
    echo "restart unexpectedly succeeded with invalid config" >&2; exit 1
fi
stop_line=$(grep -n -m1 "Daemon stopped successfully" "$root/failed-restart.out" | cut -d: -f1 || true)
fail_line=$(grep -n -m1 "sing-box configuration check: FAIL" "$root/failed-restart.out" | cut -d: -f1 || true)
[ -n "$stop_line" ] || { echo "stop_line missing from failed-restart.out" >&2; cat "$root/failed-restart.out" >&2; exit 1; }
[ -n "$fail_line" ] || { echo "fail_line missing from failed-restart.out" >&2; cat "$root/failed-restart.out" >&2; exit 1; }
[ "$stop_line" -lt "$fail_line" ]
! grep -q "Starting atpd..." "$root/failed-restart.out"
[ ! -e "$root/run/atpd.pid" ]

printf '%s\n' '=== restart uses stop then the same startup path ==='
: > "$root/commands"
run_atp start >"$root/first-start.out" 2>"$root/first-start.err"
run_atp restart >"$root/restart.out" 2>"$root/restart.err"
stop_line=$(grep -n -m1 "Daemon stopped successfully" "$root/restart.out" | cut -d: -f1 || true)
check_line=$(grep -n -m1 "Checking sing-box configuration" "$root/restart.out" | cut -d: -f1 || true)
start_line=$(grep -n -m1 "Starting atpd..." "$root/restart.out" | cut -d: -f1 || true)
success_line=$(grep -n -m1 "Daemon started successfully" "$root/restart.out" | cut -d: -f1 || true)
status_line=$(grep -n -m1 "Runtime status:" "$root/restart.out" | cut -d: -f1 || true)
[ -n "$stop_line" ] || { echo "stop_line missing from restart.out" >&2; cat "$root/restart.out" >&2; exit 1; }
[ -n "$check_line" ] || { echo "check_line missing from restart.out" >&2; cat "$root/restart.out" >&2; exit 1; }
[ -n "$start_line" ] || { echo "start_line missing from restart.out" >&2; cat "$root/restart.out" >&2; exit 1; }
[ -n "$success_line" ] || { echo "success_line missing from restart.out" >&2; cat "$root/restart.out" >&2; exit 1; }
[ -n "$status_line" ] || { echo "status_line missing from restart.out" >&2; cat "$root/restart.out" >&2; exit 1; }
[ "$stop_line" -lt "$check_line" ]
[ "$check_line" -lt "$start_line" ]
[ "$start_line" -lt "$success_line" ]
[ "$success_line" -lt "$status_line" ]
! grep -q '^tools ' "$root/commands"
pid=$(read_atpd_pid "$root/run/atpd.pid")
grep -q "Daemon started successfully (PID: $pid)" "$root/restart.out"
kill -0 "$pid"
run_atp stop >/dev/null 2>&1 || true

printf '%s\n' '=== SIGHUP reload is transactional ==='
cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
UI_EMOJI_ENABLED=1
SERVICE_START_TIMEOUT=2
SERVICE_STOP_TIMEOUT=10
SERVICE_MAX_FAILURES=5
SERVICE_HEALTH_CHECK_INTERVAL=5000
EOF
run_atp start >"$root/reload-start.out" 2>"$root/reload-start.err"
reload_pid=$(read_atpd_pid "$root/run/atpd.pid")
reload_child_pid=$(sed -n '1p' "$root/run/sing-box.pid")

wait_for_log() {
    pattern=$1
    previous=$2
    attempts=0
    while [ "$(grep -c "$pattern" "$root/run/atp.log" 2>/dev/null || true)" -le "$previous" ]; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 100 ] || {
            echo "timed out waiting for reload log: $pattern" >&2
            return 1
        }
        sleep 0.05
    done
}

# An invalid candidate is never published and does not disturb either process.
invalid_before=$(grep -c 'INVALID_CANDIDATE' "$root/run/atp.log" 2>/dev/null || true)
printf '%s\n' "DATA_DIR=$root" 'API_PORT=0' > "$root/atp.conf"
kill -HUP "$reload_pid"
wait_for_log 'INVALID_CANDIDATE' "$invalid_before"
kill -0 "$reload_pid"
kill -0 "$reload_child_pid"
[ "$(read_atpd_pid "$root/run/atpd.pid")" = "$reload_pid" ]
[ "$(sed -n '1p' "$root/run/sing-box.pid")" = "$reload_child_pid" ]
cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
UI_EMOJI_ENABLED=1
SERVICE_START_TIMEOUT=2
SERVICE_STOP_TIMEOUT=10
SERVICE_MAX_FAILURES=5
SERVICE_HEALTH_CHECK_INTERVAL=5000
EOF
run_atp status > "$root/invalid-candidate-status.out"
grep -q 'Native API (Port 19080)' "$root/invalid-candidate-status.out"
grep -q '🌐 VPN TUNNEL STATUS' "$root/invalid-candidate-status.out"

# A resolved restart-required change is rejected before any owner is applied.
restart_before=$(grep -c 'REQUIRES_RESTART' "$root/run/atp.log" 2>/dev/null || true)
cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19081
UI_EMOJI_ENABLED=0
SERVICE_START_TIMEOUT=3
SERVICE_STOP_TIMEOUT=11
SERVICE_MAX_FAILURES=6
SERVICE_HEALTH_CHECK_INTERVAL=250
EOF
kill -HUP "$reload_pid"
wait_for_log 'REQUIRES_RESTART' "$restart_before"
grep -q 'REQUIRES_RESTART (API_PORT)' "$root/run/atp.log"
kill -0 "$reload_pid"
kill -0 "$reload_child_pid"
run_atp status > "$root/restart-required-status.out"
grep -q 'Native API (Port 19080)' "$root/restart-required-status.out"
grep -q '🌐 VPN TUNNEL STATUS' "$root/restart-required-status.out"

# A hot-only candidate is applied without restarting ATPD or sing-box.
success_before=$(grep -c 'Config reload completed successfully' "$root/run/atp.log" 2>/dev/null || true)
cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
UI_EMOJI_ENABLED=0
SERVICE_START_TIMEOUT=3
SERVICE_STOP_TIMEOUT=11
SERVICE_MAX_FAILURES=6
SERVICE_HEALTH_CHECK_INTERVAL=250
EOF
kill -HUP "$reload_pid"
wait_for_log 'Config reload completed successfully' "$success_before"
[ "$(read_atpd_pid "$root/run/atpd.pid")" = "$reload_pid" ]
[ "$(sed -n '1p' "$root/run/sing-box.pid")" = "$reload_child_pid" ]
run_atp status > "$root/hot-reload-status.out"
grep -q 'State.*STANDALONE / DIRECT' "$root/hot-reload-status.out"
! grep -q '🌐 VPN TUNNEL STATUS' "$root/hot-reload-status.out"
run_atp stop >/dev/null

printf '%s\n' '=== main PID identity rejects stale and foreign processes ==='
mkdir -p "$root/identity/run"
cat > "$root/identity_helper.c" <<'EOF'
#include <unistd.h>
int main(void) {
    for (;;) pause();
}
EOF
cc -O2 -o "$root/identity/atpd" "$root/identity_helper.c"
cp "$root/identity/atpd" "$root/identity/atpd-helper"
cat > "$root/identity.conf" <<EOF
DATA_DIR=$root/identity
EOF

start_identity_process() {
    "$1" &
    identity_pid=$!
    identity_starttime=$(proc_starttime "$identity_pid")
}

write_identity() {
    printf '%s\n%s\n' "$1" "$2" > "$root/identity/run/atpd.pid"
}

# A matching PID, starttime, and exact executable name is accepted.
start_identity_process "$root/identity/atpd"
write_identity "$identity_pid" "$identity_starttime"
"$ATPD_BIN" -c "$root/identity.conf" stop >/dev/null
wait "$identity_pid" 2>/dev/null || true
[ ! -e "$root/identity/run/atpd.pid" ]

# Replacing the executable leaves the running process marked "(deleted)".
start_identity_process "$root/identity/atpd"
write_identity "$identity_pid" "$identity_starttime"
cp "$root/identity/atpd-helper" "$root/identity/atpd.new"
mv "$root/identity/atpd.new" "$root/identity/atpd"
"$ATPD_BIN" -c "$root/identity.conf" stop >/dev/null
wait "$identity_pid" 2>/dev/null || true
[ ! -e "$root/identity/run/atpd.pid" ]

# Prefix matches are not identities: atpd-helper must never receive SIGHUP.
start_identity_process "$root/identity/atpd-helper"
write_identity "$identity_pid" "$identity_starttime"
if "$ATPD_BIN" -c "$root/identity.conf" reload >/dev/null 2>&1; then
    echo "reload accepted atpd-helper" >&2; exit 1
fi
kill -0 "$identity_pid"
kill "$identity_pid"
wait "$identity_pid" 2>/dev/null || true

# A reused PID is represented by a starttime mismatch and must not be signalled.
start_identity_process "$root/identity/atpd"
write_identity "$identity_pid" "$((identity_starttime + 1))"
if "$ATPD_BIN" -c "$root/identity.conf" stop >/dev/null 2>&1; then
    echo "stop accepted mismatched starttime" >&2; exit 1
fi
kill -0 "$identity_pid"
kill "$identity_pid"
wait "$identity_pid" 2>/dev/null || true

# Legacy PID-only records are unverified and therefore fail safe.
start_identity_process "$root/identity/atpd"
printf '%s\n' "$identity_pid" > "$root/identity/run/atpd.pid"
if "$ATPD_BIN" -c "$root/identity.conf" stop >/dev/null 2>&1; then
    echo "stop accepted legacy PID-only record" >&2; exit 1
fi
kill -0 "$identity_pid"
kill "$identity_pid"
wait "$identity_pid" 2>/dev/null || true
rm -f "$root/identity/run/atpd.pid"

sh tests/test_reload_transaction_unit.sh
sh tests/test_service_health.sh

printf '%s\n' 'start/restart startup regression tests passed'
