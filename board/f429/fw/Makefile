# Cross-build the F429 slave image (ticket #17). Not part of the host ctest
# tree: CI does not require an ARM toolchain (parent #13 Testing Decisions).
#
#   mingw32-make -C board/f429/fw
#   make -C board/f429/fw
# Needs arm-none-eabi-gcc on PATH.

ROOT      := $(abspath ../../..)
BUILD     := build
TARGET    := f429_slave
CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size

MCU       := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
CFLAGS    := $(MCU) -std=c99 -Wall -Wextra -Werror -Os \
             -ffunction-sections -fdata-sections -fno-common \
             -DECSP_ENABLE_HOST=0 -DECSP_ENABLE_SLAVE=1 \
             -I$(ROOT)/include -I.
LDFLAGS   := $(MCU) -T$(abspath linker.ld) -Wl,--gc-sections \
             -nostartfiles -specs=nano.specs -specs=nosys.specs

ECSP_OBJS := \
  $(BUILD)/crc.o \
  $(BUILD)/bytes.o \
  $(BUILD)/frame.o \
  $(BUILD)/cmds.o \
  $(BUILD)/txn_cache.o \
  $(BUILD)/session.o \
  $(BUILD)/long_run.o \
  $(BUILD)/engine.o

FW_OBJS := \
  $(BUILD)/startup.o \
  $(BUILD)/clock.o \
  $(BUILD)/usart1.o \
  $(BUILD)/main.o

OBJS := $(ECSP_OBJS) $(FW_OBJS)

.PHONY: all clean

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin
	$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD):
	mkdir $(BUILD)

$(BUILD)/$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/crc.o: $(ROOT)/src/ecsp/core/crc.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/bytes.o: $(ROOT)/src/ecsp/core/bytes.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/frame.o: $(ROOT)/src/ecsp/core/frame.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/cmds.o: $(ROOT)/src/ecsp/core/cmds.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/txn_cache.o: $(ROOT)/src/ecsp/slave/txn_cache.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/session.o: $(ROOT)/src/ecsp/slave/session.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/long_run.o: $(ROOT)/src/ecsp/slave/long_run.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/engine.o: $(ROOT)/src/ecsp/slave/engine.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/startup.o: startup.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/clock.o: clock.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/usart1.o: usart1.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-cmd /c "if exist $(BUILD) rmdir /s /q $(BUILD)"
	-rm -rf $(BUILD) 2>/dev/null || true
