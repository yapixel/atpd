#!/bin/bash
#
# ATPd + sing-box Lifecycle Integration Test Script
# Test Matrix: 1. 启动 (Start) -> 2. 停止 (Stop) -> 3. 重启 (Restart) -> 4. 停止 (Final Stop)
#
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info() { echo -e "${CYAN}[TEST-INFO]${NC} $*"; }
log_pass() { echo -e "${GREEN}[TEST-PASS]${NC} $*"; }
log_fail() { echo -e "${RED}[TEST-FAIL]${NC} $*" >&2; exit 1; }
log_warn() { echo -e "${YELLOW}[TEST-WARN]${NC} $*"; }

TEST_DIR="${TMPDIR:-/tmp}/atp_lifecycle_test"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log_info "Project Root: ${PROJECT_ROOT}"
log_info "Test Directory: ${TEST_DIR}"

# 1. Clean and setup test environment
pkill -9 -f "${TEST_DIR}" 2>/dev/null || true
pkill -9 -f "sing-box.*${TEST_DIR}" 2>/dev/null || true
rm -rf "${TEST_DIR}"
mkdir -p "${TEST_DIR}/bin" "${TEST_DIR}/run"

# 2. Check atpd binary
if [ ! -f "${PROJECT_ROOT}/build/bin/atpd" ]; then
    log_info "Building atpd binary..."
    make -C "${PROJECT_ROOT}" clean
    make -C "${PROJECT_ROOT}"
fi
cp "${PROJECT_ROOT}/build/bin/atpd" "${TEST_DIR}/atpd"
chmod +x "${TEST_DIR}/atpd"

# 3. Locate sing-box binary
SINGBOX_BIN=""
if [ -n "${SINGBOX_BIN_OVERRIDE:-}" ] && [ -x "${SINGBOX_BIN_OVERRIDE}" ]; then
    SINGBOX_BIN="${SINGBOX_BIN_OVERRIDE}"
elif command -v sing-box >/dev/null 2>&1; then
    SINGBOX_BIN="$(command -v sing-box)"
elif [ -x "/usr/bin/sing-box" ]; then
    SINGBOX_BIN="/usr/bin/sing-box"
elif [ -x "/usr/local/bin/sing-box" ]; then
    SINGBOX_BIN="/usr/local/bin/sing-box"
elif [ -x "/data/data/com.termux/files/usr/bin/sing-box" ]; then
    SINGBOX_BIN="/data/data/com.termux/files/usr/bin/sing-box"
elif [ -x "/data/adb/atp/bin/sing-box" ]; then
    SINGBOX_BIN="/data/adb/atp/bin/sing-box"
fi

if [ -z "${SINGBOX_BIN}" ]; then
    log_fail "sing-box binary not found! Please install sing-box."
fi

log_info "Found sing-box binary at: ${SINGBOX_BIN}"
cp "${SINGBOX_BIN}" "${TEST_DIR}/bin/sing-box"
chmod +x "${TEST_DIR}/bin/sing-box"
EXPECTED_SINGBOX_VERSION="$("${TEST_DIR}/bin/sing-box" version | awk '/^sing-box version /{print $3; exit}')"
[ -n "${EXPECTED_SINGBOX_VERSION}" ] || log_fail "无法读取 sing-box 版本"

TEST_API_PORT="${API_PORT:-9080}"

# 4. Generate sing-box minimal config.json
cat > "${TEST_DIR}/config.json" << EOJSON
{
  "log": {
    "level": "info",
    "timestamp": true
  },
  "services": [
    {
      "type": "api",
      "listen": "127.0.0.1",
      "listen_port": ${TEST_API_PORT}
    }
  ],
  "experimental": {
    "clash_api": {}
  },
  "inbounds": [
    {
      "type": "mixed",
      "tag": "mixed-in",
      "listen": "127.0.0.1",
      "listen_port": 2080
    }
  ],
  "outbounds": [
    {
      "type": "direct",
      "tag": "direct"
    }
  ],
  "route": {
    "rules": [
      { "clash_mode": "Google VPN", "outbound": "direct" },
      { "clash_mode": "Global", "outbound": "direct" },
      { "clash_mode": "Direct", "outbound": "direct" }
    ]
  }
}
EOJSON

# 5. Generate atp.conf
cat > "${TEST_DIR}/atp.conf" << EOCONF
LOG_TIMESTAMP=1
API_PORT=${TEST_API_PORT}
SERVICE_START_TIMEOUT=15
SERVICE_STOP_TIMEOUT=10
CORE_USER_GROUP=root:root
EOCONF

log_pass "Test environment initialized successfully."

# Helper to dump logs on failure
dump_logs() {
    log_warn "--- DUMPING RUN LOGS ---"
    if [ -f "${TEST_DIR}/run/atp.log" ]; then
        echo "=== atp.log ==="
        cat "${TEST_DIR}/run/atp.log" || true
    fi
    if [ -f "${TEST_DIR}/sing-box.log" ]; then
        echo "=== sing-box.log ==="
        cat "${TEST_DIR}/sing-box.log" || true
    fi
    chmod -R 777 "${TEST_DIR}" 2>/dev/null || true
}
trap dump_logs ERR

cleanup_test_processes() {
    if [ -x "${TEST_DIR}/atpd" ]; then
        (cd "${TEST_DIR}" && ./atpd stop >/dev/null 2>&1) || true
    fi
}
trap cleanup_test_processes EXIT

wait_for_api() {
    for _ in {1..20}; do
        if nc -z 127.0.0.1 "${TEST_API_PORT}" 2>/dev/null || \
           (echo > "/dev/tcp/127.0.0.1/${TEST_API_PORT}") 2>/dev/null; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

status_values_valid() {
    local output="$1"
    local clean
    clean="$(echo "${output}" | sed -r 's/\x1b\[[0-9;]*m//g')"
    STATUS_GOROUTINES="$(echo "${clean}" | awk '/Goroutines/{print $NF; exit}')"
    STATUS_VERSION="$(echo "${clean}" | awk '/Version/{print $NF; exit}')"
    STATUS_CLASH_MODE="$(echo "${clean}" | awk '/Clash Mode/{print $NF; exit}')"
    [[ "${STATUS_GOROUTINES}" =~ ^[1-9][0-9]*$ ]] &&
        [ "${STATUS_VERSION}" = "${EXPECTED_SINGBOX_VERSION}" ] &&
        [ "${STATUS_CLASH_MODE}" = "Rule" ]
}

# --- PRE-CHECK: Validate sing-box config syntax ---
log_info "Pre-check: Validating sing-box configuration syntax..."
"${TEST_DIR}/bin/sing-box" check \
    -c "${PROJECT_ROOT}/examples/config.json.example" -D "${TEST_DIR}"
log_pass "Repository eBPF example accepted by sing-box."
"${TEST_DIR}/bin/sing-box" check -c "${TEST_DIR}/config.json" -D "${TEST_DIR}"
log_pass "sing-box configuration verified by official CLI check."

cd "${TEST_DIR}"

# ==============================================================================
# 阶段 1: 首次启动 (Start) -> 校验进程存活与 Native API 就绪
# ==============================================================================
log_info "=== [STEP 1/4] 启动测试: 'atpd start' ==="
./atpd start

# 校验 sing-box Native API 端口就绪
log_info "校验 sing-box Native API (127.0.0.1:${TEST_API_PORT})..."
API_OK=0
if wait_for_api; then API_OK=1; fi

if [ "${API_OK}" -eq 1 ]; then
    log_pass "sing-box Native API 响应正常 (Port ${TEST_API_PORT})"
else
    dump_logs
    log_fail "sing-box Native API 未能在预期时间内响应!"
fi

if [ -s "${TEST_DIR}/sing-box.log" ] && [ ! -e "${TEST_DIR}/run/sing-box.log" ]; then
    log_pass "sing-box 日志位于工作目录 ${TEST_DIR}/sing-box.log"
else
    dump_logs
    log_fail "sing-box 日志路径不符合工作目录规范"
fi

# sing-box does not create a PID file itself. ATPd writes the supervised
# child's PID in ATPd's runtime directory so CLI status/stop still work after
# a daemon re-exec on Android.
SINGBOX_PID_FILE="${TEST_DIR}/run/sing-box.pid"
if [ -s "${SINGBOX_PID_FILE}" ]; then
    SINGBOX_PID="$(tr -d '[:space:]' < "${SINGBOX_PID_FILE}")"
    if [[ "${SINGBOX_PID}" =~ ^[1-9][0-9]*$ ]] && \
       kill -0 "${SINGBOX_PID}" 2>/dev/null && \
       [ "$(tr -d '[:space:]' < "/proc/${SINGBOX_PID}/comm" 2>/dev/null)" = "sing-box" ]; then
        log_pass "ATPd 已将 sing-box PID 写入 ${SINGBOX_PID_FILE}"
    else
        dump_logs
        log_fail "sing-box PID 文件无效或未指向运行中的 sing-box"
    fi
else
    dump_logs
    log_fail "ATPd 未写入 sing-box PID 文件: ${SINGBOX_PID_FILE}"
fi

# 等待启动稳定并获取状态数据
STATUS_OUTPUT=""
for i in {1..20}; do
    STATUS_OUTPUT="$(./atpd -n status 2>&1)"
    if echo "${STATUS_OUTPUT}" | grep -q "PID" && status_values_valid "${STATUS_OUTPUT}"; then
        break
    fi
    sleep 0.5
done
echo "${STATUS_OUTPUT}"

if echo "${STATUS_OUTPUT}" | grep -q "PID"; then
    log_pass "Step 1 PASS: atpd 成功拉起 sing-box 并捕获到活跃 PID!"
else
    dump_logs
    log_fail "Step 1 FAIL: atpd 未能成功拉起 sing-box!"
fi

GOROUTINES_VALUE="$(echo "${STATUS_OUTPUT}" | sed -r 's/\x1b\[[0-9;]*m//g' | awk '/Goroutines/{print $NF; exit}')"
if [[ "${GOROUTINES_VALUE}" =~ ^[1-9][0-9]*$ ]]; then
    log_pass "Native API SubscribeStatus 返回实时 Goroutines=${GOROUTINES_VALUE}"
else
    dump_logs
    log_fail "Goroutines 未从 Native API 返回整数值: ${GOROUTINES_VALUE:-N/A}"
fi

if status_values_valid "${STATUS_OUTPUT}" && [ "${STATUS_VERSION}" = "${EXPECTED_SINGBOX_VERSION}" ]; then
    log_pass "Native API GetVersion 返回 Version=${STATUS_VERSION}"
else
    dump_logs
    log_fail "Version 未从 Native API 返回真实值"
fi

if echo "${STATUS_OUTPUT}" | sed -r 's/\x1b\[[0-9;]*m//g' | grep -qE 'Clash Mode[[:space:]]+Rule'; then
    log_pass "Native API GetClashModeStatus 返回默认模式 Rule"
else
    dump_logs
    log_fail "未能通过 Native API 读取默认 Clash mode"
fi

if echo "${STATUS_OUTPUT}" | grep -qE 'FCM|Netlink Listener|XFRM SA Listener|MONITORS & SENSING'; then
    log_fail "Removed monitor output remains"
else
    log_pass "Removed monitor output absent"
fi

# Kill only the supervised child. The daemon-owned API snapshot must become
# invalid while the endpoint is down, then republish after supervisor recovery.
OLD_SINGBOX_PID="${SINGBOX_PID}"
log_info "验证 sing-box crash 后 Native API snapshot 失效与重连恢复..."
kill -9 "${OLD_SINGBOX_PID}"

STALE_SEEN=0
for _ in {1..40}; do
    CRASH_STATUS="$(./atpd -n status 2>&1)"
    if echo "${CRASH_STATUS}" | sed -r 's/\x1b\[[0-9;]*m//g' | \
       grep -qE 'Clash Mode[[:space:]]+N/A'; then
        STALE_SEEN=1
        break
    fi
    sleep 0.25
done
[ "${STALE_SEEN}" -eq 1 ] || { dump_logs; log_fail "sing-box crash 后 owner snapshot 未失效"; }

RECOVERY_STATUS=""
NEW_SINGBOX_PID=""
for _ in {1..60}; do
    if [ -s "${SINGBOX_PID_FILE}" ]; then
        NEW_SINGBOX_PID="$(tr -d '[:space:]' < "${SINGBOX_PID_FILE}")"
    fi
    RECOVERY_STATUS="$(./atpd -n status 2>&1)"
    if [[ "${NEW_SINGBOX_PID}" =~ ^[1-9][0-9]*$ ]] && \
       [ "${NEW_SINGBOX_PID}" != "${OLD_SINGBOX_PID}" ] && \
       kill -0 "${NEW_SINGBOX_PID}" 2>/dev/null && \
       status_values_valid "${RECOVERY_STATUS}"; then
        break
    fi
    sleep 0.25
done
echo "${RECOVERY_STATUS}"
if [[ "${NEW_SINGBOX_PID}" =~ ^[1-9][0-9]*$ ]] && \
   [ "${NEW_SINGBOX_PID}" != "${OLD_SINGBOX_PID}" ] && \
   status_values_valid "${RECOVERY_STATUS}"; then
    SINGBOX_PID="${NEW_SINGBOX_PID}"
    log_pass "sing-box crash 后 owner snapshot 已失效并由新 PID ${NEW_SINGBOX_PID} 重连发布"
else
    dump_logs
    log_fail "sing-box crash recovery 未重新发布有效 Native API snapshot"
fi

# ==============================================================================
# 阶段 2: 停止一次 (Stop) -> 校验进程安全退出与状态归位
# ==============================================================================
log_info "=== [STEP 2/4] 停止测试: 'atpd stop' ==="
./atpd stop

STOP_STATUS=""
for i in {1..20}; do
    STOP_STATUS="$(./atpd -n status 2>&1)"
    if echo "${STOP_STATUS}" | grep -q "STOPPED" || echo "${STOP_STATUS}" | grep -q "Daemon stopped"; then
        break
    fi
    sleep 0.5
done
echo "${STOP_STATUS}"

if echo "${STOP_STATUS}" | grep -q "STOPPED" || echo "${STOP_STATUS}" | grep -q "Daemon stopped"; then
    log_pass "Step 2 PASS: sing-box 进程与守护中枢已完全平稳停止!"
else
    dump_logs
    log_fail "Step 2 FAIL: 停止指令执行后进程未能退出!"
fi

if [ ! -e "${SINGBOX_PID_FILE}" ]; then
    log_pass "停止后已清理 sing-box PID 文件"
else
    dump_logs
    log_fail "停止后仍残留 sing-box PID 文件: ${SINGBOX_PID_FILE}"
fi

# ==============================================================================
# 阶段 3: 再次启动 (Re-start) -> 校验新进程与 Native API snapshot
# ==============================================================================
log_info "=== [STEP 3/4] 再次启动测试: 'atpd start' ==="
./atpd start

# 等待重启后 sing-box Native API 完成监听
if wait_for_api; then
    log_pass "重启后 sing-box Native API 响应正常 (Port ${TEST_API_PORT})"
else
    dump_logs
    log_fail "重启后 sing-box Native API 无法访问!"
fi

RESTART_STATUS=""
for i in {1..20}; do
    RESTART_STATUS="$(./atpd -n status 2>&1)"
    if echo "${RESTART_STATUS}" | grep -q "PID" && status_values_valid "${RESTART_STATUS}"; then
        break
    fi
    sleep 0.5
done
echo "${RESTART_STATUS}"

if echo "${RESTART_STATUS}" | grep -q "PID" && status_values_valid "${RESTART_STATUS}"; then
    log_pass "Step 3 PASS: atpd 成功重启 sing-box 并保持健康运行!"
else
    dump_logs
    log_fail "Step 3 FAIL: 重启后 sing-box 未能恢复运行!"
fi

# ==============================================================================
# 阶段 4: 最终停止 (Final Stop) -> 完整生命周期收尾
# ==============================================================================
log_info "=== [STEP 4/4] 最终收尾: 'atpd stop' ==="
./atpd stop

FINAL_STATUS=""
for i in {1..20}; do
    FINAL_STATUS="$(./atpd -n status 2>&1)"
    if echo "${FINAL_STATUS}" | grep -q "STOPPED" || echo "${FINAL_STATUS}" | grep -q "Daemon stopped"; then
        break
    fi
    sleep 0.5
done
echo "${FINAL_STATUS}"

if echo "${FINAL_STATUS}" | grep -q "STOPPED" || echo "${FINAL_STATUS}" | grep -q "Daemon stopped"; then
    log_pass "Step 4 PASS: 完整启停、重启闭环生命周期测试全部通过 (100% SUCCESS)!"
else
    dump_logs
    log_fail "Step 4 FAIL: 最终停止后未能正确处于停止状态!"
fi

if [ -e "${SINGBOX_PID_FILE}" ]; then
    dump_logs
    log_fail "最终停止后仍残留 sing-box PID 文件: ${SINGBOX_PID_FILE}"
fi

# 清理测试现场
chmod -R 777 "${TEST_DIR}" 2>/dev/null || true
rm -rf "${TEST_DIR}"
