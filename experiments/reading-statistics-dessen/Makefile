# Build only a Dessen app. No firmware toolchain/build is invoked.
DESSEN ?= ../../dessen
RISCV ?= $(HOME)/.platformio/tools/toolchain-riscv32-esp/bin/riscv32-esp-elf-
APPFLAGS = -march=rv32im -mabi=ilp32 -Os -ffreestanding -nostdlib -mno-relax -fno-tree-loop-distribute-patterns -Wall -Wextra -I$(DESSEN)/sdk -T $(DESSEN)/sdk/app.ld -Wl,--no-warn-rwx-segments

.PHONY: all test clean
all: build/app.bin

build/app.bin: main.c $(DESSEN)/sdk/crt0.S $(DESSEN)/sdk/rt.c $(DESSEN)/sdk/dessen_calls.h
	mkdir -p build
	$(RISCV)gcc $(APPFLAGS) $(DESSEN)/sdk/crt0.S $(DESSEN)/sdk/rt.c main.c -o build/app.elf
	$(RISCV)objcopy -O binary build/app.elf $@

test: build/app.bin $(DESSEN)/build/dessen
	python3 test_fixture.py $(DESSEN)/build/dessen build/app.bin

clean:
	rm -rf build
