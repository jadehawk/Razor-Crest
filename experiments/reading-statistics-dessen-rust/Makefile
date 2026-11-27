# Standalone Rust Dessen application (never compiles firmware).
DESSEN := ../../dessen
RISCV ?= $(HOME)/.platformio/tools/toolchain-riscv32-esp/bin/riscv32-esp-elf-
.PHONY: all test
all: build/app.bin
build/dessen_compact.rs: compact_sdk.py $(DESSEN)/sdk/dessen.rs
	mkdir -p build
	python3 compact_sdk.py $(DESSEN)/sdk/dessen.rs $@
build/app.bin: Makefile save_restore.S build/dessen_compact.rs readstats.rs dashboard.rs preview.rs bookhero.rs pages.rs calendar.rs sync.rs qr.rs server.rs session_history.rs native_history.rs history_upload.rs settings.rs sleep.rs book_filters.rs $(DESSEN)/sdk/dessen.rs $(DESSEN)/sdk/crt0.S $(DESSEN)/sdk/rt.c $(DESSEN)/sdk/app.ld
	mkdir -p build
	RUSTC_BOOTSTRAP=1 rustc -Z location-detail=none -C target-feature=+relax,+save-restore -C llvm-args=-enable-machine-outliner=always --target riscv32im-unknown-none-elf --crate-type staticlib -C panic=abort -C opt-level=z -C codegen-units=1 -C lto=fat -C embed-bitcode=yes -o build/libreadstats.a readstats.rs
	$(RISCV)gcc -march=rv32im -mabi=ilp32 -nostdlib -Os -ffreestanding -fno-tree-loop-distribute-patterns -mrelax -ffunction-sections -T $(DESSEN)/sdk/app.ld -Wl,--defsym,APP_MEM=84992 $(DESSEN)/sdk/crt0.S $(DESSEN)/sdk/rt.c save_restore.S build/libreadstats.a -o build/app.elf -Wl,--gc-sections,--no-warn-rwx-segments,--no-relax-gp
	$(RISCV)objcopy -O binary build/app.elf $@
# Paint the unused guest region and enforce a 1 KiB minimum written-stack margin.
# Derive the boundary from the linked ELF, not a hardcoded binary size.
test: all
	DESSEN_TEST_STACK_START=$$($(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}') python3 test_fixture.py $(DESSEN)/build/dessen build/app.bin
	DESSEN_TEST_STACK_START=$$($(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}') python3 test_sync.py $(DESSEN)/build/dessen build/app.bin
	DESSEN_TEST_STACK_START=$$($(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}') python3 test_unpair.py $(DESSEN)/build/dessen build/app.bin
	DESSEN_TEST_STACK_START=$$($(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}') python3 test_session_history.py $(DESSEN)/build/dessen build/app.bin
	DESSEN_TEST_STACK_START=$$($(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}') python3 test_sync_state.py $(DESSEN)/build/dessen build/app.bin
	DESSEN_TEST_STACK_START=$$($(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}') python3 test_history_upload.py $(DESSEN)/build/dessen build/app.bin
	DESSEN_TEST_STACK_START=$$($(RISCV)size -A build/app.elf | awk '$$1 == ".bss" {print $$2+$$3}') python3 test_calendar_books.py $(DESSEN) build/app.bin
