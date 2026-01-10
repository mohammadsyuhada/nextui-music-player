###########################################################
# Supported platforms: tg5040, tg5050 (future)
# Default: tg5040

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
PLATFORM=tg5040
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

TARGET = musicplayer
# Paths adjusted for location in workspace/tg5040/nextui-music-player/src/
INCDIR = -I. -I./audio -I../../../all/common/ -I../../platform/ -I../../../all/minarch/libretro-common/include
INCDIR += -I./include -I./include/mbedtls_lib -I./include/helix-aac

# mbedTLS source files (for HTTPS support)
MBEDTLS_SRC = $(wildcard include/mbedtls_lib/*.c)

# Helix AAC decoder source files
HELIX_AAC_SRC = $(wildcard include/helix-aac/*.c)

SOURCE = $(TARGET).c player.c radio.c radio_net.c radio_album_art.c radio_hls.c radio_curated.c youtube.c selfupdate.c \
         ui_fonts.c ui_utils.c browser.c ui_album_art.c ui_main.c ui_music.c ui_radio.c ui_youtube.c ui_system.c \
         spectrum.c audio/kiss_fft.c audio/kiss_fftr.c \
         include/parson/parson.c \
         include/mbedtls_entropy_alt.c \
         $(MBEDTLS_SRC) \
         $(HELIX_AAC_SRC) \
         ../../../all/common/utils.c ../../../all/common/api.c ../../../all/common/config.c ../../../all/common/scaler.c \
         ../../platform/platform.c

CC = $(CROSS_COMPILE)gcc

# Reset CFLAGS to avoid issues with platform makefile.env
MY_CFLAGS = -O2 -fomit-frame-pointer
# mbedTLS config
MY_CFLAGS += -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>'
MY_CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
MY_CFLAGS += -DUSE_SDL2 -DUSE_GLES -DGL_GLEXT_PROTOTYPES
MY_CFLAGS += -I$(PREFIX)/include -I$(PREFIX_LOCAL)/include
MY_CFLAGS += -I../../libmsettings
MY_CFLAGS += -I../../wifimanager/daemon
MY_CFLAGS += -I../../btmanager/src/include

MY_LDFLAGS = -L$(PREFIX)/lib -L$(PREFIX_LOCAL)/lib
MY_LDFLAGS += -L../../libmsettings
MY_LDFLAGS += -L../../wifimanager/src/core
MY_LDFLAGS += -L../../wifimanager/daemon
MY_LDFLAGS += -L../../../all/minarch/build/$(PLATFORM)
MY_LDFLAGS += -L../../btmanager/src/lib/aarch64/glibc-gcc8_3_0
MY_LDFLAGS += -lSDL2 -lSDL2_image -lSDL2_ttf -lGLESv2 -lEGL
MY_LDFLAGS += -lmsettings -lsamplerate -lzip -lm -lpthread -ldl -lz

ifeq ($(PLATFORM), tg5040)
MY_CFLAGS += -DHAS_WIFIMG -DHAS_BTMG
MY_LDFLAGS += -lwifimg -lwifid
MY_LDFLAGS += -lbtmg -lglib-2.0 -lgio-2.0 -lshared-mainloop -lbluetooth-internal -lasound -ljson-c
endif

PRODUCT= ../$(TARGET).elf

all:
	$(CC) $(SOURCE) -o $(PRODUCT) $(MY_CFLAGS) $(MY_LDFLAGS)

clean:
	rm -f $(PRODUCT)
