/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Configuration validator implementation
 */

#include "config_validator.h"
#include <arpa/inet.h>
#include <string.h>

static const config_key_spec_t CONFIG_SCHEMA[] = {
    {"DATA_DIR", CONFIG_VALUE_STRING, false, "DATA_DIR", false},
    {"WORK_DIR", CONFIG_VALUE_STRING, true, "DATA_DIR", false},
    {"RUN_DIR", CONFIG_VALUE_STRING, false, "RUN_DIR", false},
    {"PID_FILE", CONFIG_VALUE_STRING, false, "PID_FILE", false},
    {"LOG_FILE", CONFIG_VALUE_STRING, false, "LOG_FILE", false},
    {"LOG_TIMESTAMP", CONFIG_VALUE_BOOL, false, "LOG_TIMESTAMP", false},
    {"RESTART_DELAY", CONFIG_VALUE_INT, false, "RESTART_DELAY", false},
    {"API_SECRET", CONFIG_VALUE_STRING, false, "API_SECRET", true},
    {"CLASH_SECRET", CONFIG_VALUE_STRING, true, "API_SECRET", true},
    {"API_PORT", CONFIG_VALUE_INT, false, "API_PORT", false},
    {"API_HOST", CONFIG_VALUE_STRING, false, "API_HOST", false},
    {"UI_EMOJI_ENABLED", CONFIG_VALUE_BOOL, false, "UI_EMOJI_ENABLED", false},
    {"CORE_USER_GROUP", CONFIG_VALUE_STRING, false, "CORE_USER_GROUP", false},
    {"SERVICE_START_TIMEOUT", CONFIG_VALUE_INT, false, "SERVICE_START_TIMEOUT", false},
    {"SERVICE_STOP_TIMEOUT", CONFIG_VALUE_INT, false, "SERVICE_STOP_TIMEOUT", false},
    {"SERVICE_GRACE_PERIOD", CONFIG_VALUE_INT, false, "SERVICE_GRACE_PERIOD", false},
    {"SERVICE_MAX_FAILURES", CONFIG_VALUE_INT, false, "SERVICE_MAX_FAILURES", false},
    {"SERVICE_CIRCUIT_THRESHOLD", CONFIG_VALUE_INT, false, "SERVICE_CIRCUIT_THRESHOLD", false},
    {"SERVICE_CIRCUIT_COOLDOWN", CONFIG_VALUE_INT, false, "SERVICE_CIRCUIT_COOLDOWN", false},
    {"SERVICE_HEALTH_CHECK_INTERVAL", CONFIG_VALUE_INT, false, "SERVICE_HEALTH_CHECK_INTERVAL", false},
    {"SERVICE_ARGS", CONFIG_VALUE_STRING, false, "SERVICE_ARGS", true},
    {"SERVICE_ENV", CONFIG_VALUE_STRING, false, "SERVICE_ENV", true},
    {"VPN_AUTO_CLASH_MODE", CONFIG_VALUE_BOOL, true, "VPN_AUTO_MODE", false},
    {"VPN_AUTO_MODE", CONFIG_VALUE_BOOL, false, "VPN_AUTO_MODE", false},
    {"VPN_TARGET_MODE", CONFIG_VALUE_STRING, false, "VPN_TARGET_MODE", false},
    {"VPN_CLASH_MODE", CONFIG_VALUE_STRING, true, "VPN_TARGET_MODE", false},
    {"VPN_FALLBACK_MODE", CONFIG_VALUE_STRING, false, "VPN_FALLBACK_MODE", false},
    {"VPN_DEFAULT_MODE", CONFIG_VALUE_STRING, true, "VPN_FALLBACK_MODE", false},
    {NULL, 0, false, NULL, false}
};

const config_key_spec_t *config_schema_find(const char *key) {
    if (!key || !*key) return NULL;
    for (const config_key_spec_t *spec = CONFIG_SCHEMA; spec->name; spec++) {
        if (strcmp(key, spec->name) == 0) return spec;
    }
    return NULL;
}

int config_validate_key(const char *key) {
    return config_schema_find(key) ? 0 : -1;
}

static int validate_port(int port) {
    return port >= 1 && port <= 65535 ? 0 : -1;
}

static int validate_service_params(const atp_config_t *cfg) {
    int errors = 0;

    if (cfg->service.start_timeout_sec < 1 || cfg->service.start_timeout_sec > 3600) {
        errors++;
    }
    if (cfg->service.stop_timeout_sec < 1 || cfg->service.stop_timeout_sec > 3600) {
        errors++;
    }
    if (cfg->service.grace_period_sec < 0 || cfg->service.grace_period_sec > 3600) {
        errors++;
    }
    if (cfg->service.max_failures < 1 || cfg->service.max_failures > 1000) {
        errors++;
    }
    if (cfg->service.circuit_threshold < 1 || cfg->service.circuit_threshold > 1000) {
        errors++;
    }
    if (cfg->service.circuit_cooldown_sec < 1 || cfg->service.circuit_cooldown_sec > 86400) {
        errors++;
    }
    if (cfg->service.health_check_interval_ms < 100 ||
        cfg->service.health_check_interval_ms > 3600000) {
        errors++;
    }

    return errors;
}

int config_validate_values(const atp_config_t *cfg) {
    if (!cfg) return -1;

    int errors = 0;

    errors += validate_port(cfg->api.port) ? 1 : 0;
    struct in_addr address;
    errors += inet_pton(AF_INET, cfg->api.host, &address) == 1 ? 0 : 1;
    errors += validate_service_params(cfg);

    if (cfg->service.restart_delay_sec < 0 || cfg->service.restart_delay_sec > 3600) {
        errors++;
    }
    return errors == 0 ? 0 : -1;
}
