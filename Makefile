# ATP - Advanced Transparent Proxy
# Makefile for Android NDK / Linux build (True Native Size-Optimized)

PREFIX ?= /data/adb/atp
BINDIR ?= $(PREFIX)/bin
RUNDIR ?= $(PREFIX)/run
SINGBOXDIR ?= $(PREFIX)/sing-box

ZIG_VERSION := $(strip $(shell cat .zig-version))
ifneq ($(filter 1 true yes,$(USE_CCACHE) $(CCACHE)),)
override CC := ccache zig cc
else
override CC := zig cc
endif
.DEFAULT_GOAL := all

CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -DNDEBUG -fPIC
CFLAGS += -Oz -flto -ffunction-sections -fdata-sections
CFLAGS += -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS += -fmerge-all-constants -fno-ident
CFLAGS += -fstack-protector-strong
CFLAGS += -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_FAST_FP_CONV=1 -DYYJSON_DISABLE_NON_STANDARD=1
CFLAGS += -DATP_DEFAULT_DIR=\"$(PREFIX)\"
CFLAGS += -DATP_CONF_FILE=\"atp.conf\"
CFLAGS += -DATP_PID_FILE=\"run/atpd.pid\"
CFLAGS += -DATP_LOG_FILE=\"run/atp.log\"
CFLAGS += -DATP_COMMAND_SOCKET=\"run/atpd.sock\"
CFLAGS += -Iinclude

ifdef DEBUG
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -fPIC -g -DATP_DEBUG -O0 -fsanitize=address -Iinclude
SANITIZER_LIBS = -l:libasan.so.8
LDFLAGS ?= -Wl,-z,relro,-z,now
endif
# Authoritative single definition of _FORTIFY_SOURCE hardening
ifeq ($(findstring _FORTIFY_SOURCE,$(CFLAGS)),)
ifneq ($(findstring -O0,$(CFLAGS)),-O0)
CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
endif
endif
CFLAGS += -Ibuild/generated
CFLAGS += $(EXTRA_CFLAGS)

LIBS = -lpthread $(SANITIZER_LIBS)

LDFLAGS ?= -flto -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none -Wl,-z,relro,-z,now

SRC = $(wildcard src/*.c)

OBJDIR = build/obj
OBJ = $(SRC:%.c=$(OBJDIR)/%.o)
TARGET = build/bin/atpd
VPN_MODE_TEST = build/tests/test_api_vpn_mode
API_SNAPSHOT_TEST = build/tests/test_api_snapshot
LOGGER_SAFETY_TEST = build/tests/test_logger_file_safety
RESULT_TEST = build/tests/test_atp_result
VERSION_HEADER = build/generated/version_build.h
VERSION_TEST = build/tests/test_version
CONFIG_VALUE_TEST = build/tests/test_config_value
CONTEXT_TEST = build/tests/test_atpd_context
CLI_TEST = build/tests/test_cli
STATUS_RENDER_TEST = build/tests/test_status_render
UTILS_PROC_STAT_TEST = build/tests/test_utils_proc_stat
SERVICE_CREDENTIALS_TEST = build/tests/test_service_credentials
API_HOST_TEST = build/tests/test_api_host
REACTOR_TIMERS_TEST = build/tests/test_reactor_timers

.PHONY: all test clean distclean install uninstall check-zig

check-zig: .zig-version
	@command -v zig >/dev/null 2>&1 || { echo "error: Zig $(ZIG_VERSION) is required but zig is not in PATH" >&2; exit 1; }
	@actual="$$(zig version)"; [ "$$actual" = "$(ZIG_VERSION)" ] || { echo "error: Zig $(ZIG_VERSION) is required, found $$actual" >&2; exit 1; }

all: check-zig $(TARGET)

test: check-zig $(TARGET) $(VPN_MODE_TEST) $(API_SNAPSHOT_TEST) $(LOGGER_SAFETY_TEST) $(RESULT_TEST) $(VERSION_TEST) $(CONFIG_VALUE_TEST) $(CONTEXT_TEST) $(CLI_TEST) $(STATUS_RENDER_TEST) $(UTILS_PROC_STAT_TEST) $(SERVICE_CREDENTIALS_TEST) $(API_HOST_TEST) $(REACTOR_TIMERS_TEST)
	$(VPN_MODE_TEST)
	$(API_SNAPSHOT_TEST)
	$(LOGGER_SAFETY_TEST)
	$(RESULT_TEST)
	$(VERSION_TEST)
	$(CONFIG_VALUE_TEST)
	$(CONTEXT_TEST)
	$(CLI_TEST)
	$(STATUS_RENDER_TEST)
	$(UTILS_PROC_STAT_TEST)
	$(SERVICE_CREDENTIALS_TEST)
	$(API_HOST_TEST)
	$(REACTOR_TIMERS_TEST)
	sh tests/test_config_validation.sh $(TARGET)
	sh tests/test_start_restart_preflight.sh
	sh tests/test_android_service.sh

$(VPN_MODE_TEST): tests/test_api_vpn_mode.c src/api.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(API_SNAPSHOT_TEST): tests/test_api_snapshot.c src/api.c src/reactor.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(LOGGER_SAFETY_TEST): tests/test_logger_file_safety.c src/logger.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(RESULT_TEST): tests/test_atp_result.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(VERSION_TEST): tests/test_version.c src/version.c $(VERSION_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/test_version.c src/version.c $(LIBS)

$(CONFIG_VALUE_TEST): tests/test_config_value.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(CONTEXT_TEST): tests/test_atpd_context.c src/atpd_context.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(CLI_TEST): tests/test_cli.c src/cli.c src/version.c $(VERSION_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/test_cli.c src/cli.c src/version.c $(LIBS)

$(STATUS_RENDER_TEST): tests/test_status_render.c src/status_render.c src/ui.c src/version.c $(VERSION_HEADER)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/test_status_render.c src/status_render.c src/ui.c src/version.c $(LIBS)

$(UTILS_PROC_STAT_TEST): tests/test_utils_proc_stat.c src/utils.c src/logger.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ tests/test_utils_proc_stat.c src/utils.c src/logger.c $(LIBS)

$(SERVICE_CREDENTIALS_TEST): tests/test_service_credentials.c src/service_credentials.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(API_HOST_TEST): tests/test_api_host.c src/singbox_api.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wl,-wrap,connect -o $@ $^ $(LIBS)

$(REACTOR_TIMERS_TEST): tests/test_reactor_timers.c src/reactor.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(RUNDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/atpd

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/atpd

$(TARGET): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
	@echo "  LD (Native Lean) $@"

$(VERSION_HEADER): FORCE VERSION scripts/gen_version.sh
	@bash scripts/gen_version.sh $@

FORCE:

$(OBJ): $(VERSION_HEADER)
$(OBJDIR)/src/version.o: $(VERSION_HEADER)

$(OBJ) $(VPN_MODE_TEST) $(API_SNAPSHOT_TEST) $(LOGGER_SAFETY_TEST) $(RESULT_TEST) $(VERSION_TEST) $(CONFIG_VALUE_TEST) $(CONTEXT_TEST) $(CLI_TEST) $(STATUS_RENDER_TEST) $(UTILS_PROC_STAT_TEST) $(SERVICE_CREDENTIALS_TEST) $(API_HOST_TEST) $(REACTOR_TIMERS_TEST): | check-zig

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC       $<"
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build/
	find src/ -name "*.o" -delete

distclean: clean
	rm -f $(TARGET)
