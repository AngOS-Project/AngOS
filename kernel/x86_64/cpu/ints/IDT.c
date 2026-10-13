#include <types.h>
#include <syscall.h>
#include <cpu/IDT.h>
#include <diagnostics.h>
#include <keyboard.h>

#define INTS 0x30

#include <keyboard.h>

typedef struct {
	u16 size;
	void *offset;
} __attribute__((packed)) idt_ptr;
idt_ptr IDTP;

typedef struct {
	u16 offset1;
	u16 selector;
	u8 ist;
	u8 type;
	u16 offset2;
	u32 offset3;
	u32 resv;
} __attribute__((packed)) idt_entry;
idt_entry IDT[INTS];

/*
 * AngOS Watchtower exception reporting.
 *
 * The CPU error-code vectors use a two-argument handler.
 * All panic paths log to serial and halt the system.
 */
#define WT_NOERR(name, description, vector) \
    __attribute__((interrupt)) void name(void *frame) { \
        diagnostics_panic(description, vector, frame, false, 0, false, 0); \
    }

#define WT_ERR(name, description, vector) \
    __attribute__((interrupt)) void name(void *frame, u64 error_code) { \
        diagnostics_panic(description, vector, frame, true, error_code, false, 0); \
    }

WT_NOERR(div_0_fault, "Divide error (#DE)", 0x00)
WT_NOERR(debug_fault, "Debug exception (#DB)", 0x01)
WT_NOERR(non_maskable_interrupt, "Non-maskable interrupt", 0x02)
WT_NOERR(breakpoint_fault, "Breakpoint (#BP)", 0x03)
WT_NOERR(overflow_fault, "Overflow (#OF)", 0x04)
WT_NOERR(bound_fault, "Bound range exceeded (#BR)", 0x05)
WT_NOERR(inv_opcode_fault, "Invalid opcode (#UD)", 0x06)
WT_NOERR(no_device_fault, "Device not available (#NM)", 0x07)

WT_ERR(double_fault, "Double fault (#DF)", 0x08)
WT_NOERR(reserved_9_fault, "Reserved exception vector 9", 0x09)
WT_ERR(inv_tss_fault, "Invalid TSS (#TS)", 0x0A)
WT_ERR(no_segment_fault, "Segment not present (#NP)", 0x0B)
WT_ERR(stack_fault, "Stack-segment fault (#SS)", 0x0C)
WT_ERR(gp_fault, "General protection fault (#GP)", 0x0D)

__attribute__((interrupt))
void page_fault(void *frame, u64 error_code) {
    /* Read CR2 immediately, before calling another function. */
    u64 fault_address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));

    diagnostics_panic(
        "Page fault (#PF)",
        0x0E,
        frame,
        true,
        error_code,
        true,
        fault_address
    );
}

WT_NOERR(reserved_15_fault, "Reserved exception vector 15", 0x0F)
WT_NOERR(x87_fault, "x87 floating-point fault (#MF)", 0x10)
WT_ERR(align_check_fault, "Alignment check (#AC)", 0x11)
WT_NOERR(machine_check_fault, "Machine check (#MC)", 0x12)
WT_NOERR(simd_fault, "SIMD floating-point fault (#XM)", 0x13)
WT_NOERR(virtualization_fault, "Virtualization exception (#VE)", 0x14)
WT_ERR(control_protection_fault, "Control protection fault (#CP)", 0x15)
WT_NOERR(reserved_22_fault, "Reserved exception vector 22", 0x16)
WT_NOERR(reserved_23_fault, "Reserved exception vector 23", 0x17)
WT_NOERR(reserved_24_fault, "Reserved exception vector 24", 0x18)
WT_NOERR(reserved_25_fault, "Reserved exception vector 25", 0x19)
WT_NOERR(reserved_26_fault, "Reserved exception vector 26", 0x1A)
WT_NOERR(reserved_27_fault, "Reserved exception vector 27", 0x1B)
WT_NOERR(reserved_28_fault, "Reserved exception vector 28", 0x1C)
WT_ERR(vmm_communication_fault, "VMM communication exception (#VC)", 0x1D)
WT_ERR(security_fault, "Security exception (#SX)", 0x1E)
WT_NOERR(reserved_31_fault, "Reserved exception vector 31", 0x1F)

#undef WT_NOERR
#undef WT_ERR

__attribute__((interrupt))
void nop_master(void *frame __attribute__((unused))) {
    piceoi(false);
}

__attribute__((interrupt))
void nop_slave(void *frame __attribute__((unused))) {
    piceoi(true);
}

void addirq(u8 entry, void *offset, u8 type) {
	u64 off = (u64) offset;
	idt_entry *idt = &IDT[entry];
	idt->offset1 = off & 0xFFFF;
	idt->offset2 = (off >> 16) & 0xFFFF;
	idt->offset3 = (off >> 32) & 0xFFFFFFFF;
	idt->ist = 0;
	idt->selector = 8;
	idt->type = type;
	idt->resv = 0;
}

void init_idt(void) {
    /* CPU exceptions: 0x00 through 0x1F. */
    addirq(0x00, div_0_fault, 0x8F);
    addirq(0x01, debug_fault, 0x8F);
    addirq(0x02, non_maskable_interrupt, 0x8F);
    addirq(0x03, breakpoint_fault, 0x8F);
    addirq(0x04, overflow_fault, 0x8F);
    addirq(0x05, bound_fault, 0x8F);
    addirq(0x06, inv_opcode_fault, 0x8F);
    addirq(0x07, no_device_fault, 0x8F);
    addirq(0x08, double_fault, 0x8F);
    addirq(0x09, reserved_9_fault, 0x8F);
    addirq(0x0A, inv_tss_fault, 0x8F);
    addirq(0x0B, no_segment_fault, 0x8F);
    addirq(0x0C, stack_fault, 0x8F);
    addirq(0x0D, gp_fault, 0x8F);
    addirq(0x0E, page_fault, 0x8F);
    addirq(0x0F, reserved_15_fault, 0x8F);
    addirq(0x10, x87_fault, 0x8F);
    addirq(0x11, align_check_fault, 0x8F);
    addirq(0x12, machine_check_fault, 0x8F);
    addirq(0x13, simd_fault, 0x8F);
    addirq(0x14, virtualization_fault, 0x8F);
    addirq(0x15, control_protection_fault, 0x8F);
    addirq(0x16, reserved_22_fault, 0x8F);
    addirq(0x17, reserved_23_fault, 0x8F);
    addirq(0x18, reserved_24_fault, 0x8F);
    addirq(0x19, reserved_25_fault, 0x8F);
    addirq(0x1A, reserved_26_fault, 0x8F);
    addirq(0x1B, reserved_27_fault, 0x8F);
    addirq(0x1C, reserved_28_fault, 0x8F);
    addirq(0x1D, vmm_communication_fault, 0x8F);
    addirq(0x1E, security_fault, 0x8F);
    addirq(0x1F, reserved_31_fault, 0x8F);

    /* Hardware IRQs. Keyboard IRQ1 retains its existing handler. */
    for (u8 i = 0x20; i < 0x28; ++i)
        addirq(i, nop_master, 0x8E);

    for (u8 i = 0x28; i < 0x30; ++i)
        addirq(i, nop_slave, 0x8E);

    addirq(0x21, irq1_stub, 0x8E);

    remappic(0x20, 0x28);

    IDTP.offset = IDT;
    IDTP.size = sizeof(IDT) - 1;

    __asm__ volatile("lidt (%0)" :: "r"(&IDTP));
}
