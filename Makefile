# make            speech-to-text program (needs ort/ from ./fetch.sh)
# make epdtext    e-Paper text display (needs Waveshare's C driver and liblgpio-dev, see README)
# make preview    the same text renderer without the driver; it can only write PBM previews
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
ORT     ?= ort
EPD_DIR ?= $(HOME)/e-Paper/RaspberryPi_JetsonNano/c

EPD_INC = -I$(EPD_DIR)/lib/Config -I$(EPD_DIR)/lib/e-Paper
EPD_SRC = $(EPD_DIR)/lib/e-Paper/EPD_2in13_V4.c $(EPD_DIR)/lib/Config/DEV_Config.c \
          $(EPD_DIR)/lib/Config/dev_hardware_SPI.c

gigaam: gigaam.c
	$(CC) $(CFLAGS) -I$(ORT)/include gigaam.c -o gigaam \
	    -L$(ORT)/lib -lonnxruntime -Wl,-rpath,'$$ORIGIN/$(ORT)/lib'

epdtext: epdtext.c font8x16.h
	$(CC) $(CFLAGS) -DHAVE_EPD -DUSE_LGPIO_LIB -DRPI $(EPD_INC) -c epdtext.c -o epdtext.o
	$(CC) -O2 -w -DUSE_LGPIO_LIB -DRPI $(EPD_INC) $(EPD_SRC) epdtext.o -o epdtext -llgpio -lm

preview: epdtext.c font8x16.h
	$(CC) $(CFLAGS) epdtext.c -o epdtext-preview

clean:
	rm -f gigaam epdtext epdtext-preview epdtext.o

.PHONY: clean preview
