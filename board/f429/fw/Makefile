# Cross-build the F429 slave image (ticket #18 / ADR-0009). Not part of the
# host ctest tree: CI does not require an ARM toolchain (parent #13).
#
#   mingw32-make -C board/f429/fw
#   make -C board/f429/fw
# Needs arm-none-eabi-gcc on PATH.

ROOT      := $(abspath ../../..)
BOARD     := $(abspath ..)
BUILD     := build
DRIVERS   := Drivers
TARGET    := f429_slave
CROSS     ?= arm-none-eabi-
CC        := $(CROSS)gcc
OBJCOPY   := $(CROSS)objcopy
SIZE      := $(CROSS)size

MCU       := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
DEFS      := -DSTM32F429xx -DUSE_HAL_DRIVER -DUSE_FULL_LL_DRIVER \
             -DECSP_ENABLE_HOST=0 -DECSP_ENABLE_SLAVE=1
INCLUDES  := -I$(ROOT)/include -I$(BOARD) -I. \
             -I$(DRIVERS)/CMSIS/Include \
             -I$(DRIVERS)/CMSIS/Device/ST/STM32F4xx/Include \
             -I$(DRIVERS)/STM32F4xx_HAL_Driver/Inc \
             -I$(DRIVERS)/STM32F4xx_HAL_Driver/Inc/Legacy
CFLAGS    := $(MCU) -std=c99 -Wall -Wextra -Werror -Os \
             -ffunction-sections -fdata-sections -fno-common \
             $(DEFS) $(INCLUDES)
# Vendor HAL/LL are not held to -Werror; keep warnings visible.
HAL_CFLAGS := $(MCU) -std=c99 -Wall -Wextra -Os \
              -ffunction-sections -fdata-sections -fno-common \
              $(DEFS) $(INCLUDES)
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
  $(BUILD)/system_stm32f4xx.o \
  $(BUILD)/bringup.o \
  $(BUILD)/clock.o \
  $(BUILD)/usart.o \
  $(BUILD)/flash_identity.o \
  $(BUILD)/power.o \
  $(BUILD)/uid.o \
  $(BUILD)/main.o

HAL_OBJS := \
  $(BUILD)/stm32f4xx_hal.o \
  $(BUILD)/stm32f4xx_hal_cortex.o \
  $(BUILD)/stm32f4xx_hal_rcc.o \
  $(BUILD)/stm32f4xx_hal_rcc_ex.o \
  $(BUILD)/stm32f4xx_hal_flash.o \
  $(BUILD)/stm32f4xx_hal_flash_ex.o \
  $(BUILD)/stm32f4xx_hal_pwr.o \
  $(BUILD)/stm32f4xx_hal_pwr_ex.o \
  $(BUILD)/stm32f4xx_ll_rcc.o \
  $(BUILD)/stm32f4xx_ll_gpio.o \
  $(BUILD)/stm32f4xx_ll_usart.o

OBJS := $(ECSP_OBJS) $(FW_OBJS) $(HAL_OBJS)

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
$(BUILD)/system_stm32f4xx.o: $(DRIVERS)/CMSIS/Device/ST/STM32F4xx/Source/Templates/system_stm32f4xx.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/bringup.o: $(BOARD)/bringup.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/clock.o: clock.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/usart.o: usart.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/flash_identity.o: flash_identity.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/power.o: power.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/uid.o: uid.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/stm32f4xx_hal.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_hal_cortex.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_hal_rcc.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_hal_rcc_ex.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_hal_flash.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_hal_flash_ex.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_hal_pwr.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_hal_pwr_ex.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_ll_rcc.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_rcc.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_ll_gpio.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_gpio.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@
$(BUILD)/stm32f4xx_ll_usart.o: $(DRIVERS)/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_usart.c | $(BUILD)
	$(CC) $(HAL_CFLAGS) -c $< -o $@

clean:
	-cmd /c "if exist $(BUILD) rmdir /s /q $(BUILD)"
	-rm -rf $(BUILD) 2>/dev/null || true
