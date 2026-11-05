# make                  build the single binary for this machine:   build/<cpu>/sirius
# make ARCH=aarch64     cross-compile it for a 64-bit Raspberry Pi on an x86_64 PC
# make dev              the same without the models and ONNX Runtime built in (small, fast to build; needs -m DIR)
#
# Before the first build:   ./deps.sh [aarch64]     static libraries (libsodium, opus, curl, ...)
#                           ./fetch.sh [aarch64]    ONNX Runtime and the speech models, which get built into the binary
HOST_ARCH := $(shell uname -m)
ARCH      ?= $(HOST_ARCH)
OUT        = build/$(ARCH)$(if $(filter dev,$(MAKECMDGOALS)),-dev)
DEPS       = deps/$(ARCH)

ifeq ($(ARCH),$(HOST_ARCH))
  CC       = cc
  OBJDUMP  = objdump
else ifeq ($(ARCH),aarch64)
  CC       = deps/cc-aarch64
  OBJDUMP  = aarch64-linux-gnu-objdump
else
  $(error cannot build for ARCH=$(ARCH) on $(HOST_ARCH))
endif

# the oldest glibc the binary may need (minor version): 36 = Debian 12 / Raspberry Pi OS "bookworm" and newer
MAX_GLIBC  = 36

CFLAGS    ?= -O2
CFLAGS    += -Wall -Wextra -std=gnu11 -fPIE -I$(DEPS)/include -Iort/include -Iepd
LDLIBS     = -L$(DEPS)/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lopus -logg -lsodium -llgpio -lm -lpthread -ldl

SRC        = sirius.c gigaam.c diary.c epdtext.c ortload.c age.c oggopus.c s3.c
EPD        = DEV_Config dev_hardware_SPI EPD_2in13_V4
OBJ        = $(SRC:%.c=$(OUT)/%.o) $(OUT)/embed.o $(EPD:%=$(OUT)/epd_%.o)

all: $(OUT)/sirius

$(OUT)/sirius: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS) -Wl,-z,relro,-z,now -s
	@$(OBJDUMP) -T $@ | grep -o 'GLIBC_2\.[0-9]*' | sort -t. -k2 -n -u | tail -n 1 | { read v; \
	  echo "$@: needs $${v:-no glibc symbols} at most, $$(du -h $@ | cut -f1)"; \
	  [ "$(ARCH)" != aarch64 ] || [ "$${v#GLIBC_2.}" -le $(MAX_GLIBC) ] || { echo "error: needs newer than glibc 2.$(MAX_GLIBC)"; exit 1; }; }

$(OUT)/%.o: %.c | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/epdtext.o: epdtext.c font8x16.h | $(OUT)
	$(CC) $(CFLAGS) -DHAVE_EPD -DUSE_LGPIO_LIB -DRPI -c epdtext.c -o $@

$(OUT)/diary.o: diary.c localkey.h diary.h | $(OUT)

# Waveshare's driver is compiled without our warning flags
$(OUT)/epd_%.o: epd/%.c | $(OUT)
	$(CC) -O2 -w -fPIE -DUSE_LGPIO_LIB -DRPI -I$(DEPS)/include -Iepd -c $< -o $@

# The models and ONNX Runtime go into the binary here ("make dev" leaves them out)
ifeq ($(MAKECMDGOALS),dev)
$(OUT)/embed.o: embed.c | $(OUT)
	$(CC) $(CFLAGS) -c embed.c -o $@
else
EMBEDDED = ort/lib-$(ARCH)/libonnxruntime.so $(wildcard models/*) certs/amazon-roots.pem
$(OUT)/embed.o: embed.c embed.h $(EMBEDDED) | $(OUT)
	$(CC) $(CFLAGS) -DEMBED '-DEMBED_ORT="ort/lib-$(ARCH)/libonnxruntime.so"' '-DEMBED_MODELS="models"' '-DEMBED_CERTS="certs"' -c embed.c -o $@
endif

dev: $(OUT)/sirius

$(OUT):
	mkdir -p $@

# The diary's local key is made once and kept: recordings made with one build can only be read by a build
# with the same localkey.h. `make clean` leaves it alone; delete it by hand to start over.
localkey.h:
	@echo "making a new local key in localkey.h - keep this file"
	@umask 077; { echo "static const unsigned char LOCAL_KEY[32] = {"; \
	    head -c 32 /dev/urandom | od -An -v -tx1 | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1, /g'; echo "};"; } > localkey.h

clean:
	rm -rf build

.PHONY: all dev clean
