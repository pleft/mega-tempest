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
disc/spx.smd: \
	spx_layout.s \
	spx.c \
	memfile.c \
	module.c \
	pcm.c \
	pcm-io.s \
	sfx_data.c \
	mulsi3.s \
	$(BUILD_PATH)/sp.bin
