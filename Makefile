CC := clang
AS := clang
LD := ld.lld

TARGET := x86_64-unknown-elf

CFLAGS := --target=$(TARGET) -m64 -mcmodel=kernel -mno-red-zone \
          -ffreestanding -fno-stack-protector -mgeneral-regs-only \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -Wall -Wextra \
          -Ikernel/include \
          -Ikernel/x86_64

ASFLAGS := --target=$(TARGET) -m64 -ffreestanding

BUILD := build
KERNEL := $(BUILD)/AngOS.elf
UEFI := $(BUILD)/AngOS.efi

KERNEL_C := \
    kernel/x86_64/init.c \
    kernel/x86_64/entry.c \
    kernel/x86_64/cpu/GDT/GDT.c \
    kernel/x86_64/cpu/ints/IDT.c \
    kernel/x86_64/cpu/ints/PIC.c \
    kernel/x86_64/cpu/ints/timer/PIT.c \
    kernel/x86_64/cpu/IO.c \
    kernel/x86_64/memory/paging.c \
    kernel/x86_64/syscall/syscall.c \
    kernel/x86_64/syscall/debug_log.c \
    kernel/x86_64/drivers/keyboard.c \
    kernel/memory/memory.c \
    kernel/term/terminal.c \
    kernel/libc/kernel.c \
    kernel/libc/mem.c
    kernel/disk/PIO.c \
    kernel/fs/FAT.c \
    kernel/fs/FS.c \

KERNEL_ASM := \
    kernel/x86_64/entry.s \
    kernel/x86_64/cpu/GDT/GDT.S \
    kernel/x86_64/memory/paging.asm

KERNEL_C_OBJS := $(KERNEL_C:kernel/%.c=$(BUILD)/%.o)

KERNEL_ASM_OBJS := \
    $(patsubst kernel/%.s,$(BUILD)/%_asm.o,$(filter %.s,$(KERNEL_ASM))) \
    $(patsubst kernel/%.S,$(BUILD)/%_asm.o,$(filter %.S,$(KERNEL_ASM))) \
    $(patsubst kernel/%.asm,$(BUILD)/%_asm.o,$(filter %.asm,$(KERNEL_ASM)))

KERNEL_OBJS := $(KERNEL_C_OBJS) $(KERNEL_ASM_OBJS)

.PHONY: all kernel uefi clean

all: kernel uefi

kernel: $(KERNEL)

uefi: $(UEFI)

$(KERNEL): $(KERNEL_OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	$(LD) -m elf_x86_64 -nostdlib -T kernel/linker.ld $(KERNEL_OBJS) -o $@
	@echo "Kernel linked: $@"

$(BUILD)/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%_asm.o: kernel/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD)/%_asm.o: kernel/%.asm
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

$(UEFI):
	$(MAKE) -C bootloader/uefi

clean:
	rm -rf $(BUILD)
	$(MAKE) -C bootloader/uefi clean
