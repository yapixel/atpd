/*
 * ATP - Advanced Transparent Proxy
 * Copyright (C) 2024-2026 ATP Project
 *
 * Initialization phases - ATPD control plane
 */

#include "config.h"
#include "atpd_init.h"
#include "logger.h"
#include "utils.h"
#include "netlink.h"
#include "service.h"
#include "api.h"
#include "cli.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static void cleanup_reactor(atpd_init_context_t *ctx) {
    if (ctx->reactor) {
        reactor_destroy(ctx->reactor);
        ctx->reactor = NULL;
    }
}

static void cleanup_netlink(atpd_init_context_t *ctx) {
    (void)ctx;
    netlink_cleanup();
}

static void cleanup_service(atpd_init_context_t *ctx) {
    if (ctx->service) {
        service_destroy(ctx->service);
        ctx->service = NULL;
    }
}

static void cleanup_api(atpd_init_context_t *ctx) {
    if (ctx->service) ctx->service->api = NULL;
    if (ctx->api) {
        atpd_set_vpn_mode_callback(NULL, NULL);
        api_cleanup(ctx->api);
    }
}

static init_phase_config_t init_phases[] = {
    {INIT_PHASE_CONFIG, "config", atpd_init_phase_config, NULL, 1},
    {INIT_PHASE_LOGGER, "logger", atpd_init_phase_logger, NULL, 1},
    {INIT_PHASE_REACTOR, "reactor", atpd_init_phase_reactor, cleanup_reactor, 1},
    {INIT_PHASE_NETLINK, "netlink", atpd_init_phase_netlink, cleanup_netlink, 1},
    {INIT_PHASE_SERVICE, "service", atpd_init_phase_service, cleanup_service, 1},
    {INIT_PHASE_API, "api", atpd_init_phase_api, cleanup_api, 0},
    {INIT_PHASE_READY, "ready", atpd_init_phase_ready, NULL, 1},
};

int atpd_init_phase_config(atpd_init_context_t *ctx) {
    if (ctx->config_loaded) {
        LOG_DEBUG("Configuration already loaded by command dispatcher");
        return 0;
    }

    LOG_INFO("Loading configuration...");
    
    char auto_cfg_path[PATH_MAX];
    const char *config_path = ctx->opts->config_file;
    if (!config_path || !config_path[0]) {
        if (snprintf(auto_cfg_path, sizeof(auto_cfg_path), "%s/%s",
                     ctx->config->core.data_dir, ATP_CONF_FILE) >= (int)sizeof(auto_cfg_path)) {
            LOG_ERROR("Configuration path is too long");
            return -1;
        }
        config_path = auto_cfg_path;
    }
    
    if (access(config_path, R_OK) == 0) {
        if (config_load(config_path, ctx->config) != ATP_OK) {
            LOG_ERROR("Invalid configuration: %s", config_path);
            return -1;
        }
    } else if (ctx->opts->config_file[0]) {
        LOG_ERROR("Cannot read configuration: %s", config_path);
        return -1;
    }

    ctx->config_loaded = true;
    return 0;
}

int atpd_init_phase_logger(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing logger...");
    
    logger_init();
    if (ctx->config->core.log_file[0]) {
        char log_path[PATH_MAX];
        if (ctx->config->core.log_file[0] == '/') {
            snprintf(log_path, sizeof(log_path), "%s", ctx->config->core.log_file);
        } else {
            if (snprintf(log_path, sizeof(log_path), "%s/%s",
                         ctx->config->core.data_dir[0] ? ctx->config->core.data_dir : ".",
                         ctx->config->core.log_file) >= (int)sizeof(log_path)) {
                LOG_ERROR("Log path is too long");
                return -1;
            }
        }
        char log_dir[PATH_MAX];
        snprintf(log_dir, sizeof(log_dir), "%s", log_path);
        char *slash = strrchr(log_dir, '/');
        if (slash) {
            *slash = '\0';
            if (mkdir_recursive(log_dir, 0755) != 0 && errno != EEXIST) {
                LOG_ERROR("Failed to create log directory: %s", strerror(errno));
                return -1;
            }
        }
        log_set_file(log_path);
    }
    log_level_t level = LOG_LEVEL_INFO;
    if (ctx->opts->verbosity == CLI_VERBOSITY_VERBOSE) level = LOG_LEVEL_DEBUG;
    if (ctx->opts->verbosity == CLI_VERBOSITY_QUIET) level = LOG_LEVEL_ERROR;
    log_set_level(level);
    if (ctx->opts->no_color) {
        log_set_color(0);
    }
    
    return 0;
}

int atpd_init_phase_reactor(atpd_init_context_t *ctx) {
    if (!ctx || ctx->reactor) return -1;

    ctx->reactor = reactor_create();
    if (!ctx->reactor) {
        LOG_ERROR("Failed to create reactor");
        return -1;
    }
    return 0;
}

int atpd_init_phase_netlink(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing Netlink & Multi-VPN tunnel listener...");
    
    if (netlink_init(NULL, ctx->config) < 0) {
        LOG_ERROR("Failed to initialize netlink");
        return -1;
    }
    
    if (!ctx->reactor) {
        netlink_cleanup();
        return -1;
    }
    netlink_set_reactor(ctx->reactor);
    if (netlink_xfrm_init(ctx->reactor) < 0) {
        netlink_cleanup();
        return -1;
    }
    
    return 0;
}

int atpd_init_phase_service(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing sing-box core service supervisor...");
    
    ctx->service = malloc(sizeof(service_ctx_t));
    if (!ctx->service) {
        LOG_ERROR("Failed to allocate service context");
        return -1;
    }
    
    if (service_init(ctx->service, ctx->config) < 0) {
        LOG_ERROR("Failed to initialize service");
        service_destroy(ctx->service);
        ctx->service = NULL;
        return -1;
    }

    if (!ctx->reactor || service_set_reactor(ctx->service, ctx->reactor) < 0) {
        service_destroy(ctx->service);
        ctx->service = NULL;
        return -1;
    }
    
    return 0;
}

int atpd_init_phase_api(atpd_init_context_t *ctx) {
    LOG_INFO("Initializing sing-box Native API client...");
    
    if (api_init(ctx->api, ctx->config) < 0) {
        api_cleanup(ctx->api);
        return -1;
    }
    atpd_set_vpn_mode_callback(api_vpn_mode_callback, ctx->api);
    if (!ctx->reactor || api_start_with_reactor(ctx->api, ctx->reactor) < 0) {
        atpd_set_vpn_mode_callback(NULL, NULL);
        api_cleanup(ctx->api);
        return -1;
    }
    if (ctx->service) ctx->service->api = ctx->api;
    
    return 0;
}

int atpd_init_phase_ready(atpd_init_context_t *ctx) {
    (void)ctx;
    LOG_INFO("ATPD control plane ready - all components initialized");
    return 0;
}

int atpd_init_run(atpd_init_context_t *ctx) {
    if (!ctx) return -1;

    ctx->completed_phases = 0;
    ctx->degraded_phases = 0;
    int failed_phase = INIT_PHASE_MAX;
    
    for (int i = 0; i < INIT_PHASE_MAX; i++) {
        init_phase_config_t *phase = &init_phases[i];
        
        if (phase->handler(ctx) != 0) {
            if (phase->required) {
                LOG_ERROR("Required phase '%s' failed", phase->name);
                failed_phase = i;
                break;
            } else {
                LOG_WARN("Optional phase '%s' failed, continuing", phase->name);
                ctx->degraded_phases |= 1u << phase->phase;
            }
        } else {
            ctx->completed_phases |= 1u << phase->phase;
        }
    }
    
    if (failed_phase != INIT_PHASE_MAX) {
        atpd_init_rollback(ctx, failed_phase);
        return -1;
    }
    
    return 0;
}

int atpd_init_rollback(atpd_init_context_t *ctx, init_phase_t phase) {
    (void)phase;
    if (!ctx) return -1;

    LOG_WARN("Rolling back completed initialization phases");
    for (int i = INIT_PHASE_MAX - 1; i >= 0; i--) {
        if (!(ctx->completed_phases & (1u << i))) continue;
        if (init_phases[i].cleanup) {
            init_phases[i].cleanup(ctx);
        }
        ctx->completed_phases &= ~(1u << i);
    }
    return 0;
}
