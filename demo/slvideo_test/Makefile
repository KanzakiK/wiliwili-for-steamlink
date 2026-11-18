# 用法（在 WSL 里）:
#   source ~/steamlink-sdk/setenv.sh
#   cd /mnt/d/bilibili-steamlink/demo/slvideo_test
#   make
#
# setenv.sh 会提供交叉工具链与 MARVELL_ROOTFS。

CROSS ?= armv7a-cros-linux-gnueabi-
CC := $(CROSS)gcc
ROOTFS ?= $(MARVELL_ROOTFS)

ifeq ($(ROOTFS),)
$(error 未找到 MARVELL_ROOTFS，请先 source ~/steamlink-sdk/setenv.sh)
endif

CFLAGS ?= -O2 -Wall -Wextra -std=c11 -DEGL_API_FB
LDFLAGS := -L$(ROOTFS)/usr/lib \
           -Wl,-rpath-link,$(ROOTFS)/usr/lib \
           -Wl,-rpath-link,$(ROOTFS)/lib
LDLIBS := -lSLVideo -lSLAudio -lSDL2 -lEGL -lGLESv2 -lm -lpthread

all: slvideo_test

slvideo_test: main.c
	$(CC) --sysroot=$(ROOTFS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f slvideo_test

.PHONY: all clean
