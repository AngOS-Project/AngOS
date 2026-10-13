#include <types.h>
#include <cpu/IO.h>
#include <diagnostics.h>

#define WT_COM1 0x3F8
#define WT_LSR  (WT_COM1 + 5)

static volatile bool wt_serial_ready;

static void wt_put_raw(char c) {
    /*
     * Poll with a limit. A missing serial device must never
     * hold the kernel forever just because a log was requested.
     */
    for (u32 spin = 0; spin < 1000000U; ++spin) {
        if (inb(WT_LSR) & 0x20) {
            outb(WT_COM1, (u8)c);
            return;
        }
    }
}

static void wt_putc(char c) {
    if (c == '\n')
        wt_put_raw('\r');

    wt_put_raw(c);
}

static void wt_write(const char *text) {
    if (!text)
        return;

    while (*text)
        wt_putc(*text++);
}

static void wt_hex64(u64 value) {
    static const char digits[] = "0123456789ABCDEF";

    wt_write("0x");

    for (int shift = 60; shift >= 0; shift -= 4)
        wt_putc(digits[(value >> shift) & 0x0F]);
}

static void wt_field_hex(const char *label, u64 value) {
    wt_write(label);
    wt_write(": ");
    wt_hex64(value);
    wt_putc('\n');
}

void diagnostics_init(void) {
    if (wt_serial_ready)
        return;

    /*
     * Initialize COM1 as an 8-N-1 serial port at 38400 baud.
     * No heap allocation, framebuffer or filesystem is used.
     */
    outb(WT_COM1 + 1, 0x00);
    outb(WT_COM1 + 3, 0x80);
    outb(WT_COM1 + 0, 0x03);
    outb(WT_COM1 + 1, 0x00);
    outb(WT_COM1 + 3, 0x03);
    outb(WT_COM1 + 2, 0xC7);
    outb(WT_COM1 + 4, 0x0B);

    wt_serial_ready = true;

    wt_write("[WATCHTOWER] Serial diagnostics initialized.\n");
}

void diagnostics_log(const char *message) {
    if (!wt_serial_ready)
        diagnostics_init();

    if (!message)
        return;

    wt_write("[WATCHTOWER] ");
    wt_write(message);
    wt_putc('\n');
}

__attribute__((noreturn))
void diagnostics_panic(
    const char *reason,
    u8 vector,
    const void *frame,
    bool has_error_code,
    u64 error_code,
    bool has_fault_address,
    u64 fault_address
) {
    /*
     * A panic is terminal for this boot. Disable maskable
     * interrupts and avoid malloc, disk I/O and terminal drawing.
     */
    __asm__ volatile("cli" ::: "memory");

    if (!wt_serial_ready)
        diagnostics_init();

    wt_write("\n");
    wt_write("========================================\n");
    wt_write(" ANGOS WATCHTOWER - KERNEL PANIC\n");
    wt_write("========================================\n");

    wt_write("Reason: ");
    wt_write(reason ? reason : "Unspecified CPU exception");
    wt_putc('\n');

    wt_field_hex("Vector", vector);

    /*
     * x86-64 interrupt frame begins with RIP, CS and RFLAGS.
     * Read only these first three saved fields; do not assume
     * that RSP and SS were pushed for a same-privilege fault.
     */
    if (frame) {
        const u64 *saved = (const u64 *)frame;

        wt_field_hex("RIP", saved[0]);
        wt_field_hex("CS", saved[1]);
        wt_field_hex("RFLAGS", saved[2]);
    } else {
        wt_write("Register frame unavailable.\n");
    }

    if (has_error_code)
        wt_field_hex("Error code", error_code);

    if (has_fault_address)
        wt_field_hex("Fault address (CR2)", fault_address);

    if (vector == 0x0E && has_error_code) {
        wt_write("Page fault: ");

        wt_write((error_code & 1)
            ? "protection violation"
            : "page not present");

        wt_write((error_code & 2)
            ? ", write access"
            : ", read access");

        wt_write((error_code & 4)
            ? ", user mode"
            : ", supervisor mode");

        if (error_code & 8)
            wt_write(", reserved page-table bit");

        if (error_code & 16)
            wt_write(", instruction fetch");

        wt_putc('\n');
    }

    wt_write("System halted. Save this serial log for debugging.\n");

    for (;;)
        __asm__ volatile("cli; hlt");
}
