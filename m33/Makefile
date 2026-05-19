CROSS    ?= arm-none-eabi-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size
OBJDUMP  := $(CROSS)objdump

ARCHFLAGS := -mcpu=cortex-m33 -mthumb -mfloat-abi=soft

CFLAGS  := $(ARCHFLAGS) -O2 -g3 -ffunction-sections -fdata-sections \
           -Wall -Wextra -Wno-unused-parameter -std=c11 -nostartfiles \
           -fno-common -Isrc $(CFLAGS_EXTRA)
LDFLAGS := $(ARCHFLAGS) -Tm33.ld -nostartfiles -Wl,--gc-sections \
           -Wl,-Map=bench.map

SRCS := \
    src/startup.c \
    src/saturn_registers.c \
    src/saturn_interp.c \
    src/workload.c \
    src/keyboard.c \
    src/thumb2_emit.c \
    src/thumb2_selftest.c \
    src/saturn_jit.c \
    src/jit_helpers.c \
    src/jit_dispatch.c \
    src/bench_main.c

OBJS := $(SRCS:.c=.o)

QEMU       ?= qemu-system-arm
QEMU_FLAGS := -M mps2-an505 -cpu cortex-m33 -nographic \
              -semihosting-config enable=on,target=native

all: bench.elf

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

bench.elf: $(OBJS) m33.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

run: bench.elf
	$(QEMU) $(QEMU_FLAGS) -kernel bench.elf

disasm: bench.elf
	$(OBJDUMP) -d bench.elf > bench.dis

clean:
	rm -f $(OBJS) bench.elf bench.map bench.dis

.PHONY: all run disasm clean
