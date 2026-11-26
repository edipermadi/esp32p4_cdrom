# Build and flash esp32p4_cdrom with arduino-cli.
#
#   make                 compile
#   make upload          compile and flash
#   make monitor         open the serial console (115200)
#   make flash           upload, then monitor
#   make clean           remove the build directory
#
# Overrides, e.g.:  make upload PORT=/dev/ttyUSB1 CHIP=postv3

ARDUINO_CLI ?= $(shell command -v arduino-cli 2>/dev/null || \
                 echo /opt/arduino-ide/resources/app/lib/backend/resources/arduino-cli)

# UART USB-C port of the JC-ESP32P4-M3 (CH340). Not the high-speed OTG port.
PORT  ?= $(firstword $(wildcard /dev/serial/by-id/usb-1a86_*) /dev/ttyUSB0)
BAUD  ?= 115200
UPLOAD_SPEED ?= 921600

# prev3 = chip rev 1.x (most M3 boards), postv3 = rev 3.x
CHIP      ?= prev3
PARTITION ?= default
DEBUG     ?= none

BOARD_OPTS := ChipVariant=$(CHIP),USBMode=default,CDCOnBoot=default,MSCOnBoot=default,\
UploadMode=default,FlashSize=16M,PartitionScheme=$(PARTITION),PSRAM=enabled,\
UploadSpeed=$(UPLOAD_SPEED),DebugLevel=$(DEBUG)
FQBN := esp32:esp32:esp32p4:$(subst $(eval) ,,$(BOARD_OPTS))

SKETCH    := $(CURDIR)
BUILD_DIR := $(CURDIR)/build
SOURCES   := $(wildcard *.ino *.cpp *.h)
BIN       := $(BUILD_DIR)/esp32p4_cdrom.ino.bin

.PHONY: all compile upload monitor flash clean info

all: compile

compile: $(BIN)

$(BIN): $(SOURCES) Makefile
	$(ARDUINO_CLI) compile --fqbn '$(FQBN)' --build-path '$(BUILD_DIR)' --warnings default '$(SKETCH)'

upload: $(BIN)
	$(ARDUINO_CLI) upload --fqbn '$(FQBN)' --port '$(PORT)' --input-dir '$(BUILD_DIR)' '$(SKETCH)'

monitor:
	$(ARDUINO_CLI) monitor --port '$(PORT)' --config baudrate=$(BAUD)

flash: upload monitor

clean:
	rm -rf '$(BUILD_DIR)'

info:
	@echo "CLI  : $(ARDUINO_CLI)"
	@echo "FQBN : $(FQBN)"
	@echo "PORT : $(PORT)"
