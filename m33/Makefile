CROSS    ?= arm-none-eabi-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
SIZE     := $(CROSS)size
OBJDUMP  := $(CROSS)objdump

ARCHFLAGS := -mcpu=cortex-m33 -mthumb -mfloat-abi=soft

CFLAGS  := $(ARCHFLAGS) -O2 -g3 -ffunction-sections -fdata-sections \
           -Wall -Wextra -Wno-unused-parameter -std=c11 -nostartfiles \
           -fno-common -Isrc $(CFLAGS_EXTRA)
JIT_CACHE_SIZE ?=
ifeq ($(strip $(JIT_CACHE_SIZE)),)
  LDFLAGS_CACHE :=
else
  LDFLAGS_CACHE := -Wl,--defsym=_jit_cache_size=$(JIT_CACHE_SIZE)
endif

LDFLAGS := $(ARCHFLAGS) -Tm33.ld -nostartfiles -Wl,--gc-sections \
           -Wl,-Map=bench.map $(LDFLAGS_CACHE)

SRCS := \
    src/startup.c \
    src/saturn_registers.c \
    src/saturn_interp.c \
    src/workload.c \
    src/workload_nqueens.c \
    src/keyboard.c \
    src/thumb2_emit.c \
    src/thumb2_selftest.c \
    src/saturn_jit.c \
    src/jit_helpers.c \
    src/jit_dispatch.c \
    src/bench_main.c

OBJS := $(SRCS:.c=.o)

# arm.elf — standalone smoke for the ARMv4T interpreter. Independent of
# the Saturn pipeline; rebuilds quickly.
ARM_SRCS := \
    src/startup.c \
    src/arm_interp.c \
    src/arm_main.c
ARM_OBJS := $(ARM_SRCS:.c=.arm.o)

QEMU       ?= qemu-system-arm
QEMU_FLAGS := -M mps2-an505 -cpu cortex-m33 -nographic \
              -semihosting-config enable=on,target=native

all: bench.elf

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.arm.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

bench.elf: $(OBJS) m33.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

arm.elf: $(ARM_OBJS) m33.ld
	$(CC) $(ARM_OBJS) $(ARCHFLAGS) -Tm33.ld -nostartfiles -Wl,--gc-sections \
	    -Wl,-Map=arm.map -o $@
	$(SIZE) $@

run: bench.elf
	$(QEMU) $(QEMU_FLAGS) -kernel bench.elf

run-arm: arm.elf
	$(QEMU) $(QEMU_FLAGS) -kernel arm.elf

disasm: bench.elf
	$(OBJDUMP) -d bench.elf > bench.dis

clean:
	rm -f $(OBJS) $(ARM_OBJS) bench.elf bench.map bench.dis arm.elf arm.map

.PHONY: all run run-arm disasm clean
