# 用法（在 WSL 里）:
#   source ~/steamlink-sdk/setenv.sh
#   cd /mnt/d/bilibili-steamlink/demo/ffmpeg_stream
#   make
#
# 依赖:
#   ~/steamlink-ffmpeg 的最小 FFmpeg
#   SDK rootfs 里的 libcurl

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
LDLIBS := -Wl,--start-group $(FFLIBS) -Wl,--end-group \
          -lSLVideo -lSLAudio -lSDL2 -lcurl -lm -lpthread

all: ffmpeg_stream

ffmpeg_stream: main.c
	$(CC) --sysroot=$(ROOTFS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f ffmpeg_stream

.PHONY: all clean
