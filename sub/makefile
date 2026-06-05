PROJECT_ID:=mod_play
TARGET:=MEGACD
PROJECT_NAME="MOD PLAYER"
REGION?=US
VIDEO?=NTSC

SRC_PATH:=src
RES_PATH:=res
BUILD_PATH:=build
DISC_PATH:=disc

CC_FLAGS?=-O1 -fconserve-stack -fomit-frame-pointer -fno-gcse
AS_FLAGS?=
LD_FLAGS?=

MEGADEV_PATH?=/opt/megadev

include $(MEGADEV_PATH)/megadev.make

.PHONY: init all clean

init:
	@mkdir -p $(SRC_PATH) $(RES_PATH) $(BUILD_PATH) $(DISC_PATH)

all: disc/spx.smd

clean:
	@rm -f $(DISC_PATH)/*.smd $(BUILD_PATH)/* > /dev/null 2>&1

# Sub CPU module. Includes the cart bridge (spx.c + memfile.c) plus the
# upstream MOD player (module.c) and RF5C164 driver (pcm.c + pcm-io.s),
# plus megadev's standard sp.bin sub bootstrap.
#
# std_init.s MUST come before spx.c — it provides the `init` entry label
# at module offset 0 which zeroes .bss and copies .data ROM→RAM before
# jumping to main. Our Mode 1 cart's mcd_init PCs the sub to offset
# $10000 (= the start of this module), so whatever's first in .init is
# what runs first. Previously we ran main directly with uninitialised
# statics — the megadev graphics ASIC engine may depend on this init.
disc/spx.smd: \
	spx_layout.s \
	std_init.s \
	spx.c \
	memfile.c \
	module.c \
	pcm.c \
	pcm-io.s \
	mulsi3.s \
	$(BUILD_PATH)/sp.bin
