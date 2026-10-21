# BMS firmware - one source tree, two targets.
#
#   make [TARGET=posix]     FreeRTOS POSIX simulator      -> build/posix/bms_posix
#   make TARGET=qemu        Cortex-M3 (QEMU mps2-an385)   -> build/qemu/: bootloader.elf,
#                           app_slotA.elf, app_slotB.elf (+ .bin, .map, size report) and
#                           the signed images app_slotA.signed.bin, app_slotB.signed.bin
#                           (VERSION=x.y.z sets their version; a dev signing key is
#                           generated in keys/ on first use)
#   make test               Unity unit tests on the host (ASan + UBSan), independent of TARGET
#   make run   [TARGET=..]  run the firmware (posix: on $(CAN_IF); qemu: bootloader + Slot A,
#                           QEMU + slcand -> slcan0)
#   make debug TARGET=qemu  start QEMU halted with a GDB server on :1234
#   make smoke TARGET=qemu  headless boot check: wait for "BMS ready" on the log UART
#   make scenario [TARGET=..]  pytest end-to-end scenarios (vcan0 or slcan0)
#   make sim                interactive battery simulator
#   make cppcheck           static analysis of both ports and the bootloader
#   make vcan               create vcan0 (needs sudo)

TARGET  ?= posix
CAN_IF  ?= $(if $(filter qemu,$(TARGET)),slcan0,vcan0)
PYTHON  ?= python3
HOST_CC ?= cc
BUILD   := build/$(TARGET)
# Release version of the application images (major.minor.patch).
VERSION ?= 1.0.0
VERSION_NUM := $(shell echo $(VERSION) | awk -F. '{ printf "0x%06X", $$1 * 65536 + $$2 * 256 + $$3 }')
# Development signing key (tools/keygen.py). Never committed: keys/ is ignored.
DEV_KEY ?= keys/dev_ed25519.pem

FREERTOS_DIR := third_party/FreeRTOS-Kernel
UNITY_DIR    := third_party/Unity
MONO_DIR     := third_party/Monocypher
# Monocypher: SHA-512 and Ed25519 (its optional module), for image signatures.
MONO_SRC     := $(MONO_DIR)/src/monocypher.c $(MONO_DIR)/src/optional/monocypher-ed25519.c
MONO_INC     := -isystem $(MONO_DIR)/src -isystem $(MONO_DIR)/src/optional

# ---- Flags shared by both targets ----------------------------------------
# Our code: strict C11, warnings are errors.
WARN    := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
           -Wformat=2 -Wundef -Wcast-align -Wpointer-arith
CSTD    := -std=c11
DEPFLAGS = -MMD -MP
APP_INC := -Iapp/include -Iapp/logic -Iapp/tasks -Icommon/include
# Simulator and QEMU builds enable test hooks; a production build would not.
FW_DEFS ?= -DBMS_ENABLE_TEST_HOOKS=1
# Extra defines for special builds (see test-images).
EXTRA_DEFS ?=
FW_DEFS += $(EXTRA_DEFS)
# Fatal errors reset the ECU; FATAL_HALT=1 stops instead, for a debugger.
FATAL_HALT ?= 0

COMMON_SRC := $(wildcard common/src/*.c)
LOGIC_SRC  := $(wildcard app/logic/*.c)
TASKS_SRC  := $(wildcard app/tasks/*.c)

# ---- Per-target settings -------------------------------------------------
ifeq ($(TARGET),posix)
  CC         ?= gcc
  PORT_DIR   := port/posix
  RTOS_PORT  := $(FREERTOS_DIR)/portable/ThirdParty/GCC/Posix
  OPT        ?= -O2 -g
  ARCH       := -pthread
  KERNEL_SRC := $(FREERTOS_DIR)/tasks.c $(FREERTOS_DIR)/queue.c $(FREERTOS_DIR)/list.c \
                $(RTOS_PORT)/port.c $(RTOS_PORT)/utils/wait_for_event.c
  FW_BIN     := $(BUILD)/bms_posix
  LDFLAGS_FW := -pthread
else ifeq ($(TARGET),qemu)
  CROSS      ?= arm-none-eabi-
  CC         := $(CROSS)gcc
  SIZE       := $(CROSS)size
  OBJCOPY    := $(CROSS)objcopy
  PORT_DIR   := port/qemu-mps2
  RTOS_PORT  := $(FREERTOS_DIR)/portable/GCC/ARM_CM3
  OPT        ?= -Os -g
  ARCH       := -mcpu=cortex-m3 -mthumb
  KERNEL_SRC := $(FREERTOS_DIR)/tasks.c $(FREERTOS_DIR)/queue.c $(FREERTOS_DIR)/list.c \
                $(FREERTOS_DIR)/stream_buffer.c $(RTOS_PORT)/port.c
  FW_BIN     := $(BUILD)/app_slotA.elf
  FW_DEFS    += -DBMS_IMAGE_VERSION=$(VERSION_NUM) -DBMS_FATAL_HALT=$(FATAL_HALT)
  # -nostartfiles: our startup code replaces crt0. nano.specs: newlib-nano, a
  # small C library (we only use its string functions and snprintf).
  LDFLAGS_ARM := $(ARCH) -nostartfiles --specs=nano.specs -Wl,--gc-sections -Wl,--cref \
                 -Wl,--print-memory-usage
else
  $(error TARGET must be 'posix' or 'qemu')
endif

PORT_SRC := $(wildcard $(PORT_DIR)/*.c)
# Third-party headers via -isystem: their internal warnings are not ours to fix.
RTOS_INC := -I$(PORT_DIR) -isystem $(FREERTOS_DIR)/include -isystem $(RTOS_PORT)

CFLAGS_FW     := $(CSTD) $(OPT) $(ARCH) $(WARN) $(FW_DEFS) $(APP_INC) $(RTOS_INC) $(MONO_INC) \
                 -ffunction-sections -fdata-sections
# FreeRTOS kernel + port: vendor code, built with GNU extensions and without
# -Werror (we pin a release tag instead of patching it).
CFLAGS_KERNEL := -std=gnu11 $(OPT) $(ARCH) -Wall -Wno-unused-parameter -I$(PORT_DIR) \
                 -I$(FREERTOS_DIR)/include -I$(RTOS_PORT) -ffunction-sections -fdata-sections
# Other vendor code (Monocypher): same idea.
CFLAGS_VENDOR := $(CSTD) $(OPT) $(ARCH) -Wall $(MONO_INC) -ffunction-sections -fdata-sections

# Kernel and vendor objects do not depend on FW_DEFS, so special builds
# (test-images) can reuse the main build's instead of recompiling them.
SHARED_BUILD ?= $(BUILD)
KRN_OBJ    := $(patsubst %.c,$(SHARED_BUILD)/kernel/%.o,$(KERNEL_SRC))
VENDOR_OBJ := $(patsubst %.c,$(SHARED_BUILD)/vendor/%.o,$(MONO_SRC))

# ---- Unit tests (always the host compiler) -------------------------------
# Only pure logic is linked into tests: if app/logic ever includes FreeRTOS,
# the test build fails, which enforces the "no OS calls in logic" rule.
SAN          ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS_TEST  := $(CSTD) -O0 -g $(SAN) -Wall -Wextra -Werror -Wshadow $(APP_INC) $(MONO_INC) \
                -I$(UNITY_DIR)/src -pthread
TEST_SRC     := $(wildcard tests/test_*.c)
TEST_BINS    := $(patsubst tests/%.c,build/tests/%,$(TEST_SRC))
# Everything the tests link, compiled once into a static library.
TEST_LIB_SRC := $(COMMON_SRC) $(LOGIC_SRC) $(MONO_SRC) $(UNITY_DIR)/src/unity.c
TEST_LIB_OBJ := $(patsubst %.c,build/tests/obj/%.o,$(TEST_LIB_SRC))
TEST_LIB     := build/tests/libbms_test.a

.PHONY: all test run debug smoke size scenario sim cppcheck vcan clean check-submodules test-images

all: check-submodules

check-submodules:
	@test -f $(FREERTOS_DIR)/tasks.c -a -f $(MONO_DIR)/src/monocypher.c || \
	    { echo "Submodules missing: run 'git submodule update --init'"; exit 1; }

$(BUILD)/fw/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_FW) $(DEPFLAGS) -c $< -o $@

$(SHARED_BUILD)/kernel/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KERNEL) $(DEPFLAGS) -c $< -o $@

$(SHARED_BUILD)/vendor/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_VENDOR) $(DEPFLAGS) -c $< -o $@

ifeq ($(TARGET),posix)
# ==== POSIX simulator =======================================================
FW_OBJ := $(patsubst %.c,$(BUILD)/fw/%.o,$(COMMON_SRC) $(LOGIC_SRC) $(TASKS_SRC) $(PORT_SRC))
ALL_OBJ := $(FW_OBJ) $(KRN_OBJ) $(VENDOR_OBJ)

all: $(FW_BIN)

$(FW_BIN): $(FW_OBJ) $(KRN_OBJ) $(VENDOR_OBJ)
	$(CC) $(LDFLAGS_FW) -o $@ $^

size: $(FW_BIN)
	size $(FW_BIN)

run: all
	./$(FW_BIN) $(CAN_IF)

else
# ==== Cortex-M3: bootloader + application linked for Slot A and Slot B =====
# Application and common code do not depend on the slot, so they are
# compiled once; only the port (memory_map.h users) and the link differ.
APP_OBJ  := $(patsubst %.c,$(BUILD)/fw/%.o,$(COMMON_SRC) $(LOGIC_SRC) $(TASKS_SRC))
BOOT_SRC := $(wildcard bootloader/*.c) $(PORT_DIR)/uart_cmsdk.c \
            $(addprefix common/src/,bms_image.c boot_meta.c crc32.c flash_emul.c)
BOOT_OBJ := $(patsubst %.c,$(BUILD)/boot/%.o,$(BOOT_SRC))
# The public half of the signing key, as C, linked into bootloader and app.
PUBKEY_C   := $(BUILD)/gen/signing_pubkey.c
PUBKEY_OBJ := $(BUILD)/gen/signing_pubkey.o
ALL_OBJ  := $(APP_OBJ) $(KRN_OBJ) $(VENDOR_OBJ) $(BOOT_OBJ) $(PUBKEY_OBJ) \
            $(patsubst %.c,$(BUILD)/slotA/%.o,$(PORT_SRC)) $(patsubst %.c,$(BUILD)/slotB/%.o,$(PORT_SRC))

# The linker scripts go through the C preprocessor so they can include
# memory_map.h - the same addresses the C code sees.
PREPROCESS_LD = $(CC) -E -P -undef -x c -I$(PORT_DIR)

# $(1) = slot letter, $(2) = extra defines for that slot
define APP_SLOT_RULES
$(BUILD)/slot$(1)/%.o: %.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS_FW) $(2) $$(DEPFLAGS) -c $$< -o $$@

$(BUILD)/slot$(1)/link.ld: $(PORT_DIR)/mps2_an385.ld.S $(PORT_DIR)/memory_map.h
	@mkdir -p $$(dir $$@)
	$$(PREPROCESS_LD) $(2) $$< -o $$@

$(BUILD)/app_slot$(1).elf: $(APP_OBJ) $(KRN_OBJ) $(VENDOR_OBJ) $(PUBKEY_OBJ) \
                           $(patsubst %.c,$(BUILD)/slot$(1)/%.o,$(PORT_SRC)) $(BUILD)/slot$(1)/link.ld
	$$(CC) $$(LDFLAGS_ARM) -T$(BUILD)/slot$(1)/link.ld -Wl,-Map=$(BUILD)/app_slot$(1).map \
	    -o $$@ $$(filter %.o,$$^)
	@$$(SIZE) $$@
endef
$(eval $(call APP_SLOT_RULES,A,))
$(eval $(call APP_SLOT_RULES,B,-DBMS_APP_SLOT_B))

# Bootloader: bare metal, no FreeRTOS; shares memory_map.h, the register
# header and the polled UART driver with the port.
CFLAGS_BOOT := $(CSTD) $(OPT) $(ARCH) $(WARN) -Ibootloader -I$(PORT_DIR) -Icommon/include $(MONO_INC) \
               -ffunction-sections -fdata-sections

$(BUILD)/boot/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_BOOT) $(DEPFLAGS) -c $< -o $@

$(BUILD)/boot/link.ld: bootloader/bootloader.ld.S $(PORT_DIR)/memory_map.h
	@mkdir -p $(dir $@)
	$(PREPROCESS_LD) $< -o $@

$(BUILD)/bootloader.elf: $(BOOT_OBJ) $(VENDOR_OBJ) $(PUBKEY_OBJ) $(BUILD)/boot/link.ld
	$(CC) $(LDFLAGS_ARM) -T$(BUILD)/boot/link.ld -Wl,-Map=$(BUILD)/bootloader.map -o $@ $(filter %.o,$^)
	@$(SIZE) $@
	@$(SIZE) $@ | awk 'NR == 2 { n = $$1 + $$2; printf "bootloader: %d of 65536 bytes of BOOT used (%.1f%%)\n", n, n * 100 / 65536 }'

# Raw images: exactly what gets programmed into the slot / boot region.
$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

# Signing. The key is created on first use and never overwritten; see
# keys/README.md for why a production key would live in an HSM instead.
$(DEV_KEY):
	$(PYTHON) tools/keygen.py --out $@

# Only the public key reaches the firmware build.
$(PUBKEY_C): $(DEV_KEY) tools/keygen.py
	$(PYTHON) tools/keygen.py --export-c $@ --key $(DEV_KEY)

$(PUBKEY_OBJ): $(PUBKEY_C)
	$(CC) $(CFLAGS_BOOT) -c $< -o $@

$(BUILD)/app_slot%.signed.bin: $(BUILD)/app_slot%.bin $(DEV_KEY) tools/sign_image.py tools/bms_image.py
	$(PYTHON) tools/sign_image.py --key $(DEV_KEY) --version $(VERSION) $< -o $@

QEMU_IMAGES := $(BUILD)/bootloader.elf $(BUILD)/bootloader.bin \
               $(BUILD)/app_slotA.elf $(BUILD)/app_slotA.bin $(BUILD)/app_slotA.signed.bin \
               $(BUILD)/app_slotB.elf $(BUILD)/app_slotB.bin $(BUILD)/app_slotB.signed.bin

all: $(QEMU_IMAGES)

size: $(QEMU_IMAGES)
	$(SIZE) -A -x $(BUILD)/bootloader.elf $(BUILD)/app_slotA.elf

run: all
	bash scripts/run_qemu.sh --iface $(CAN_IF)

debug: all
	bash scripts/run_qemu.sh --iface $(CAN_IF) --debug

smoke: all
	bash scripts/qemu_smoke.sh

# Deliberately broken - but properly signed - v2.0.0 releases for Slot B, for
# the rollback scenarios: one never confirms itself, one crashes after 1.5 s.
TEST_VARIANTS := noconfirm:-DBMS_TEST_NO_CONFIRM crash:-DBMS_TEST_CRASH_AFTER_MS=1500
test-images: all
	@set -e; mkdir -p $(BUILD)/test-images; for v in $(TEST_VARIANTS); do \
	    name=$${v%%:*}; defs=$${v#*:}; \
	    $(MAKE) --no-print-directory TARGET=qemu BUILD=build/qemu-$$name SHARED_BUILD=$(BUILD) EXTRA_DEFS="$$defs" VERSION=2.0.0 \
	        build/qemu-$$name/app_slotB.signed.bin; \
	    cp build/qemu-$$name/app_slotB.signed.bin $(BUILD)/test-images/app_slotB_$$name.signed.bin; \
	done

# Record the captioned demo video (dashboard/ + scripts/demo_video.py) and
# post-process it with ffmpeg into docs/demo.{mp4,gif} and a thumbnail. Needs
# the Python packages in sim/requirements.txt plus playwright + imageio-ffmpeg
# (and a one-off `python -m playwright install chromium`).
demo-video: all
	$(MAKE) --no-print-directory TARGET=qemu BUILD=build/qemu-demo-nc SHARED_BUILD=$(BUILD) \
	    EXTRA_DEFS=-DBMS_TEST_NO_CONFIRM build/qemu-demo-nc/app_slotA.bin
	$(PYTHON) scripts/demo_video.py
endif

build/tests/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_TEST) $(DEPFLAGS) -c $< -o $@

# Vendor code in tests: sanitizers yes, our -Werror no.
build/tests/obj/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(filter-out -Werror -Wshadow,$(CFLAGS_TEST)) $(DEPFLAGS) -c $< -o $@

$(TEST_LIB): $(TEST_LIB_OBJ)
	rm -f $@
	ar rcs $@ $^

build/tests/%: tests/%.c $(TEST_LIB)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS_TEST) -o $@ $< $(TEST_LIB)

test: check-submodules $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

scenario: all
	cd sim && BMS_TARGET=$(TARGET) BMS_CAN_IF=$(CAN_IF) $(PYTHON) -m pytest -v tests

sim:
	cd sim && $(PYTHON) -m bms_sim --channel $(CAN_IF)

# cppcheck over our own code only (one run per port, each with its own
# FreeRTOSConfig.h); exits non-zero on any finding.
CPPCHECK := cppcheck --std=c11 --enable=warning,style,performance,portability --inline-suppr \
            --check-level=exhaustive --error-exitcode=1 --quiet -DBMS_ENABLE_TEST_HOOKS=1 \
            --suppress=missingIncludeSystem --suppress=unusedFunction \
            -Iapp/include -Iapp/logic -Iapp/tasks -Icommon/include
CPPCHECK_VENDOR := -I$(MONO_DIR)/src -I$(MONO_DIR)/src/optional --suppress='*:$(MONO_DIR)/*'
cppcheck:
	$(CPPCHECK) -D__linux__ -Iport/posix $(CPPCHECK_VENDOR) app common port/posix
	$(CPPCHECK) -D__arm__ -D__ARM_ARCH_7M__ -Iport/qemu-mps2 $(CPPCHECK_VENDOR) app common port/qemu-mps2
	$(CPPCHECK) -D__arm__ -D__ARM_ARCH_7M__ -Ibootloader -Iport/qemu-mps2 $(CPPCHECK_VENDOR) bootloader

vcan:
	sudo bash scripts/setup_vcan.sh vcan0

clean:
	rm -rf build

-include $(ALL_OBJ:.o=.d) $(TEST_LIB_OBJ:.o=.d)
