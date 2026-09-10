#!/bin/sh
set -eu

ATPD_BIN="${1:-build/bin/atpd}"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT HUP INT TERM

printf '%s\n' 'API_PORT=0' > "$TEST_TMP/invalid.conf"
if "$ATPD_BIN" -c "$TEST_TMP/invalid.conf" check >/dev/null 2>&1; then
    echo "invalid explicit configuration was accepted" >&2
    exit 1
fi

expect_invalid() {
    if "$ATPD_BIN" -c "$TEST_TMP/invalid.conf" check >/dev/null 2>&1; then
        echo "invalid configuration was accepted" >&2
        exit 1
    fi
}

printf '%s\n' 'API_P0RT=9080' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_PORT=abc' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'SERVICE_START_TIMEOUT=30x' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'VPN_AUTO_MODE=2' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'UI_EMOJI_ENABLED=-1' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_PORT=9080' 'API_PORT=9090' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%*s\n' 64 '' | tr ' ' x | sed 's/^/API_HOST=/' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_PORT 9080' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_HOST="127.0.0.1' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_HOST=' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_HOST=192.0.2.999' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_HOST=localhost' > "$TEST_TMP/invalid.conf"
expect_invalid
printf '%s\n' 'API_HOST=::1' > "$TEST_TMP/invalid.conf"
expect_invalid

mkdir "$TEST_TMP/data"
printf '%s\n' "DATA_DIR=$TEST_TMP/data" > "$TEST_TMP/invalid.conf"
printf '%s\n' '{"api":{"port":0}}' > "$TEST_TMP/data/config.json"
expect_invalid
printf '%s\n' '{"services":[{"type":"api","listen":"192.0.2.999:9080"}]}' > "$TEST_TMP/data/config.json"
expect_invalid

if "$ATPD_BIN" -c "$TEST_TMP/missing.conf" check >/dev/null 2>&1; then
    echo "missing explicit configuration was accepted" >&2
    exit 1
fi

printf '%s\n' 'API_PORT=9080' > "$TEST_TMP/valid.conf"
"$ATPD_BIN" -c "$TEST_TMP/valid.conf" check >/dev/null
printf '%s\n' 'API_HOST=192.0.2.1' > "$TEST_TMP/valid.conf"
"$ATPD_BIN" -c "$TEST_TMP/valid.conf" check >/dev/null
echo "configuration validation tests passed"
