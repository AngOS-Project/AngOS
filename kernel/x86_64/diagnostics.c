#include <types.h>
#include <cpu/IO.h>
#include <diagnostics.h>

#define WT_COM1 0x3F8
#define WT_LSR  (WT_COM1 + 5)

static volatile bool wt_serial_ready;
static volatile bool wt_serial_checked;

static void wt_put_raw(char c) {
    if (!wt_serial_ready)
        return;
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

static void wt_dump_control_registers(void) {
    u64 cr0;
    u64 cr3;
    u64 cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    wt_field_hex("CR0", cr0);
    wt_field_hex("CR3", cr3);
    wt_field_hex("CR4", cr4);
}

#define WT_HISTORY_CAPACITY 32
#define WT_HISTORY_TEXT 96
static char wt_history[WT_HISTORY_CAPACITY][WT_HISTORY_TEXT];
static u32 wt_history_count;
static u32 wt_history_next;
static bool wt_initialized;

static void diagnostics_record(const char *message) {
    u32 i = 0;
    if (!message)
        message = "(null)";
    while (i < WT_HISTORY_TEXT - 1 && message[i]) {
        wt_history[wt_history_next][i] = message[i];
        ++i;
    }
    wt_history[wt_history_next][i] = '\0';
    wt_history_next = (wt_history_next + 1) % WT_HISTORY_CAPACITY;
    if (wt_history_count < WT_HISTORY_CAPACITY)
        ++wt_history_count;
}

void diagnostics_init(void) {
    wt_initialized = true;
    if (wt_serial_checked)
        return;
    wt_serial_checked = true;

    outb(WT_COM1 + 1, 0x00);
    outb(WT_COM1 + 3, 0x80);
    outb(WT_COM1 + 0, 0x03);
    outb(WT_COM1 + 1, 0x00);
    outb(WT_COM1 + 3, 0x03);
    outb(WT_COM1 + 2, 0xC7);
    outb(WT_COM1 + 4, 0x1E);
    outb(WT_COM1, 0xAE);
    if (inb(WT_COM1) != 0xAE) {
        outb(WT_COM1 + 4, 0x00);
        return;
    }
    outb(WT_COM1 + 4, 0x0B);
    wt_serial_ready = true;
    wt_write("[WATCHTOWER] Serial diagnostics initialized.\n");
}

void diagnostics_log(const char *message) {
    diagnostics_record(message);
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

    wt_dump_control_registers();

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

    if ((vector == 0x0A || vector == 0x0B || vector == 0x0C || vector == 0x0D) && has_error_code) {
        wt_write("Selector fault: ");
        wt_write((error_code & 1) ? "external event, " : "internal event, ");
        if (error_code & 2)
            wt_write("IDT entry index ");
        else if (error_code & 4)
            wt_write("LDT selector index ");
        else
            wt_write("GDT selector index ");
        wt_hex64((error_code >> 3) & 0x1FFF);
        wt_putc('\n');
    }

    wt_write("System halted. Save this serial log for debugging.\n");

    for (;;)
        __asm__ volatile("cli; hlt");
}
u32 diagnostics_event_count(void) {
    return wt_history_count;
}

u32 diagnostics_event_capacity(void) {
    return WT_HISTORY_CAPACITY;
}

bool diagnostics_get_event(u32 index, char *buffer, u32 capacity) {
    if (!buffer || capacity == 0 || index >= wt_history_count)
        return false;
    u32 oldest = (wt_history_next + WT_HISTORY_CAPACITY - wt_history_count) % WT_HISTORY_CAPACITY;
    u32 slot = (oldest + index) % WT_HISTORY_CAPACITY;
    u32 i = 0;
    while (i + 1 < capacity && i < WT_HISTORY_TEXT - 1 && wt_history[slot][i]) {
        buffer[i] = wt_history[slot][i];
        ++i;
    }
    buffer[i] = '\0';
    return true;
}

void diagnostics_clear_events(void) {
    for (u32 i = 0; i < WT_HISTORY_CAPACITY; ++i) {
        for (u32 j = 0; j < WT_HISTORY_TEXT; ++j)
            wt_history[i][j] = '\0';
    }
    wt_history_count = 0;
    wt_history_next = 0;
}

bool diagnostics_is_initialized(void) {
    return wt_initialized;
}

bool diagnostics_selftest(void) {
    const char expected[] = "Watchtower self-test event verified";
    char actual[WT_HISTORY_TEXT];
    diagnostics_log(expected);
    u32 count = diagnostics_event_count();
    if (count == 0 || !diagnostics_get_event(count - 1, actual, sizeof(actual)))
        return false;
    u32 i = 0;
    while (expected[i] && actual[i] == expected[i])
        ++i;
    return expected[i] == '\0' && actual[i] == '\0';
}
