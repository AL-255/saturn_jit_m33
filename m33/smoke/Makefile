CROSS    ?= arm-none-eabi-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size

# Cortex-M33: ARMv8-M Mainline, no FP for now to keep it simple.
ARCHFLAGS := -mcpu=cortex-m33 -mthumb -mfloat-abi=soft

CFLAGS  := $(ARCHFLAGS) -Os -g3 -ffunction-sections -fdata-sections \
           -Wall -Wextra -std=c11 -nostartfiles -fno-common
LDFLAGS := $(ARCHFLAGS) -Tm33.ld -nostartfiles -Wl,--gc-sections -Wl,-Map=smoke.map

OBJS := startup.o main.o

all: smoke.elf

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

smoke.elf: $(OBJS) m33.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

run: smoke.elf
	qemu-system-arm -M mps2-an505 -cpu cortex-m33 -nographic \
	    -semihosting-config enable=on,target=native \
	    -kernel smoke.elf

clean:
	rm -f *.o *.elf *.map

.PHONY: all run clean
