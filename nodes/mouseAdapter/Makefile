PROJECT=mouseAdapter

ROOT ?= ../..
include $(ROOT)/setenv.mk

CC = gcc
DEFAULTFLAGS = -Wall 
CFLAGS = -O2 $(DEFAULTFLAGS)
CFLAGSUNOPT = -O0 $(DEFAULTFLAGS)
CFLAGS+ = -g -O2 $(DEFAULTFLAGS)

LINKS= -L$(HIREDIS_PATH) -L$(ROOT)/lib/nxjson/nxjson.h
LDPATH = -Wl,-rpath=$(HIREDIS_PATH)
LDFLAGS = -lhiredis

CINCLUDE = -I$(ROOT)/lib/c_code/brand/ \
		   -I$(HIREDIS_PATH) \
		   -I$(ROOT)/lib/nxjson


CFILES = $(ROOT)/lib/c_code/brand/brand.c \
		 $(ROOT)/lib/nxjson/nxjson.c

TARGET=$(PROJECT).bin

all: mouseAdapter

mouseAdapter: mouseAdapter.c
	$(CC) $(CFLAGS) $(CINCLUDE) $(LINKS) mouseAdapter.c -pthread $(CFILES) $(LDPATH) -o $(TARGET) $(LDFLAGS)

clean:
	$(RM) $(TARGET)
