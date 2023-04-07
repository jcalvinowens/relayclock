OBJCOPY = arm-none-eabi-objcopy
CC = arm-none-eabi-gcc

INCLUDES = -I./include

CFLAGS = -Wall -Wextra -Wdeclaration-after-statement -Wstrict-prototypes \
	-g -O2 -fno-strict-aliasing -nostdlib -mcpu=cortex-m0 -mthumb \
	-march=armv6-m -mlittle-endian -DSTM32F030 #-DOLD_PB9_BUTTON

LDFLAGS = -Tlink.ld

binary = relayclock.bin
elfout = relayclock.elf
obj = firmware.o start.o
asm = $(obj:.o=.s)

all: $(binary)

disasm: $(asm)
disasm: CFLAGS += -fverbose-asm

$(binary): $(elfout)
	$(OBJCOPY) -O binary $< $@

$(elfout): $(obj)
	$(CC) $(CFLAGS) $(LDFLAGS) $(obj) -o $@

%.o: %.c
	$(CC) $< $(CFLAGS) $(INCLUDES) -c -o $@

%.o: %.s
	$(CC) $< $(CFLAGS) $(INCLUDES) -c -o $@

%.s: %.c
	$(CC) $< $(CFLAGS) $(INCLUDES) -c -S -o $@

clean:
	rm -f firmware.s firmware.o start.o relayclock.bin relayclock.elf
