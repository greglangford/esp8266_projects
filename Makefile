BUILD_ROOT = build
BIN_PATH = bin

SERIAL_PORT = /dev/ttyUSB0
SERIAL_BAUD = 115200

ESPRESSIF_SDK = /opt/esp-open-sdk/ESP8266_NONOS_SDK_V2.0.0_16_08_10
XTENSA_TOOLCHAIN = /opt/esp-open-sdk/xtensa-lx106-elf

CC = $(XTENSA_TOOLCHAIN)/bin/xtensa-lx106-elf-gcc
CXX = $(XTENSA_TOOLCHAIN)/bin/xtensa-lx106-elf-g++
LD = $(XTENSA_TOOLCHAIN)/bin/xtensa-lx106-elf-gcc

CPREPROFLAGS = -D__ets__ -DICACHE_FLASH -U__STRICT_ANSI__ -I$(ESPRESSIF_SDK)/include -I$(XTENSA_TOOLCHAIN)/xtensa-lx106-elf/include \
	-I$(ESPRESSIF_SDK)/driver_lib/include

CFLAGS = -c -Os -g -Wpointer-arith -Wno-implicit-function-declaration \
	-Wl,-EL -fno-inline-functions -nostdlib -mlongcalls -mtext-section-literals \
	-falign-functions=4 -MMD -std=gnu99 -ffunction-sections -fdata-sections

ELFFLAGS = -g -Os -nostdlib -Wl,--no-check-sections -u call_user_start \
-L$(ESPRESSIF_SDK)/lib -L$(ESPRESSIF_SDK)/ld -L$(XTENSA_TOOLCHAIN)/xtensa-lx106-elf/lib -Teagle.app.v6.ld

ELFLIBS = -Wl,--start-group -lmain -lnet80211 -lwpa -llwip -lpp -lphy -lc -Wl,--end-group -lgcc

INCLUDES = -I./include \
	-I./drivers/include

MODULES = user driver mqtt

BUILD_DIR := $(addprefix $(BUILD_ROOT)/,$(MODULES))

SRC_DIR	:= $(MODULES)
SRC	:= $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.c))

OBJS = $(patsubst %.c,$(BUILD_ROOT)/%.o,$(SRC))

vpath %.c $(SRC_DIR)

define compile-objects
$1/%.o: %.c
	$(vecho) "CC $$<"
	$(Q) $(CC) $(INCDIR) $(MODULE_INCDIR) $(EXTRA_INCDIR) $(SDK_INCDIR) $(CFLAGS)  -c $$< -o $$@
endef


.PHONY: all

all: clean dirs hex

clean:
	@rm -rf ./build
	@rm -rf ./$(BIN_PATH)

dirs: $(BUILD_DIR)

elf: $(BIN_PATH)/app.out

hex: elf
	esptool.py elf2image --output $(BIN_PATH)/ $(BIN_PATH)/app.out

flash: hex
	esptool.py --port $(SERIAL_PORT) write_flash 0x00000 $(BIN_PATH)/0x00000.bin 0x10000 $(BIN_PATH)/0x10000.bin

test: clean dirs flash
	picocom $(SERIAL_PORT) -b $(SERIAL_BAUD)

refresh:
	esptool.py --port $(SERIAL_PORT) erase_flash
	esptool.py --port $(SERIAL_PORT) write_flash 0x3fc000 $(ESPRESSIF_SDK)/bin/esp_init_data_default.bin

$(BUILD_DIR):
	mkdir -p $@

$(BIN_PATH)/app.out: $(OBJS)
	$(LD) $(ELFLIBS) $(ELFFLAGS) -o $(BIN_PATH)/app.out $(OBJS)
