PROJECT_ID:=tempest
TARGET:=MEGADRIVE
PROJECT_NAME:="TEMPEST 2000"
REGION?=US
VIDEO?=NTSC

SRC_PATH:=src
RES_PATH:=res
BUILD_PATH:=build

CC_FLAGS?=-O1 -fconserve-stack -fomit-frame-pointer -fno-gcse
AS_FLAGS?=
LD_FLAGS?=

MEGADEV_PATH?=/opt/megadev
include $(MEGADEV_PATH)/megadev.make

.PHONY: init all clean

init:
	@mkdir -p $(SRC_PATH) $(RES_PATH) $(BUILD_PATH)

all: $(PROJECT_ID).cart
rom: $(PROJECT_ID).cart

clean:
	@rm -f $(PROJECT_ID).cart $(BUILD_PATH)/* > /dev/null 2>&1

$(PROJECT_ID).cart: \
	init.s \
	main.c \
	mulsi3.s \
	res.s
