# Standalone Rust Dessen application (never compiles firmware).
DESSEN := ../../dessen
RISCV ?= $(HOME)/.platformio/tools/toolchain-riscv32-esp/bin/riscv32-esp-elf-
.PHONY: all test
all: build/app.bin
build/app.bin: readstats.rs dashboard.rs preview.rs bookhero.rs pages.rs calendar.rs sync.rs qr.rs server.rs settings.rs $(DESSEN)/sdk/dessen.rs $(DESSEN)/sdk/crt0.S
	mkdir -p build
	rustc --target riscv32im-unknown-none-elf --crate-type staticlib -C panic=abort -C opt-level=z -C codegen-units=1 -o build/libreadstats.a readstats.rs
	$(RISCV)gcc -march=rv32im -mabi=ilp32 -nostdlib -mno-relax -ffunction-sections -T $(DESSEN)/sdk/app.ld -Wl,--defsym,APP_MEM=81920 $(DESSEN)/sdk/crt0.S build/libreadstats.a -o build/app.elf -Wl,--gc-sections,--no-warn-rwx-segments
	$(RISCV)objcopy -O binary build/app.elf $@
test: all
	python3 test_fixture.py $(DESSEN)/build/dessen build/app.bin
	python3 test_sync.py $(DESSEN)/build/dessen build/app.bin
	python3 test_unpair.py $(DESSEN)/build/dessen build/app.bin
