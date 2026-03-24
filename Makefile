CC = gcc
CFLAGS = -O2 -Wall -Wno-unused-result
SRCS = playpt3.c ayumi.c pt3player.c load_text.c visualizer.c
TARGET = playpt3

ifeq ($(OS),Windows_NT)
LIBS = -lwinmm -lm
else
PULSE_OK := $(shell pkg-config --exists libpulse-simple 2>/dev/null && echo yes)
ALSA_OK := $(shell pkg-config --exists alsa 2>/dev/null && echo yes)
ifeq ($(PULSE_OK),yes)
CFLAGS += -DUSE_PULSE $(shell pkg-config --cflags libpulse-simple)
LIBS = $(shell pkg-config --libs libpulse-simple) -lm -lpthread
else ifeq ($(ALSA_OK),yes)
CFLAGS += -DUSE_ALSA $(shell pkg-config --cflags alsa)
LIBS = $(shell pkg-config --libs alsa) -lm -lpthread
else
LIBS = -lm -lpthread
endif
endif

all: $(TARGET)

$(TARGET): $(SRCS) ayumi.h pt3player.h load_text.h visualizer.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET) $(TARGET).exe
