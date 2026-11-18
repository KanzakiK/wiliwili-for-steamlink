# 用法（在 WSL 里）:
#   source ~/steamlink-sdk/setenv.sh
#   cd /mnt/d/bilibili-steamlink/demo/ffmpeg_player
#   make
#
# 依赖已交叉编译并安装到 ~/steamlink-ffmpeg 的最小 FFmpeg。

CROSS ?= armv7a-cros-linux-gnueabi-
CC := $(CROSS)gcc
ROOTFS ?= $(MARVELL_ROOTFS)
FFMPEG_PREFIX ?= $(HOME)/steamlink-ffmpeg

ifeq ($(ROOTFS),)
$(error 未找到 MARVELL_ROOTFS，请先 source ~/steamlink-sdk/setenv.sh)
endif

CFLAGS ?= -O2 -Wall -Wextra -std=c11
CFLAGS += -I$(FFMPEG_PREFIX)/include
LDFLAGS := -L$(FFMPEG_PREFIX)/lib \
           -L$(ROOTFS)/usr/lib \
           -Wl,-rpath-link,$(ROOTFS)/usr/lib \
           -Wl,-rpath-link,$(ROOTFS)/lib
FFLIBS := -lavformat -lavcodec -lswresample -lavutil
LDLIBS := -Wl,--start-group $(FFLIBS) -Wl,--end-group -lSLVideo -lSLAudio -lm -lpthread

all: ffmpeg_player

ffmpeg_player: main.c
	$(CC) --sysroot=$(ROOTFS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f ffmpeg_player

.PHONY: all clean
