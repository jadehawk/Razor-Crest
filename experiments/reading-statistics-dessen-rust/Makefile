# Standalone Rust Dessen application (never compiles firmware).
DESSEN := ../../dessen
RISCV ?= $(HOME)/.platformio/tools/toolchain-riscv32-esp/bin/riscv32-esp-elf-
.PHONY: all test
all: build/app.bin
build/app.bin: readstats.rs dashboard.rs preview.rs bookhero.rs pages.rs calendar.rs sync.rs qr.rs server.rs session_history.rs settings.rs $(DESSEN)/sdk/dessen.rs $(DESSEN)/sdk/crt0.S
	mkdir -p build
	rustc --target riscv32im-unknown-none-elf --crate-type staticlib -C panic=abort -C opt-level=z -C codegen-units=1 -C lto=fat -C embed-bitcode=yes -o build/libreadstats.a readstats.rs
	$(RISCV)gcc -march=rv32im -mabi=ilp32 -nostdlib -mno-relax -ffunction-sections -T $(DESSEN)/sdk/app.ld -Wl,--defsym,APP_MEM=82944 $(DESSEN)/sdk/crt0.S build/libreadstats.a -o build/app.elf -Wl,--gc-sections,--no-warn-rwx-segments
	$(RISCV)objcopy -O binary build/app.elf $@
# Paint the unused guest region and enforce a 1 KiB minimum written-stack margin.
# Derive the boundary from the linked ELF, not a hardcoded binary size.
test: export DESSEN_TEST_STACK_START = $(shell $(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}')
test: all
	python3 test_fixture.py $(DESSEN)/build/dessen build/app.bin
	python3 test_sync.py $(DESSEN)/build/dessen build/app.bin
	python3 test_unpair.py $(DESSEN)/build/dessen build/app.bin
	python3 test_session_history.py $(DESSEN)/build/dessen build/app.bin
	python3 test_sync_state.py $(DESSEN)/build/dessen build/app.bin
