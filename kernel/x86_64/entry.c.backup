#include <keyboard.h>
#include <terminal.h>
#include <cpu/IO.h>

static int strcmp_local(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

void execute_command(const char *cmd) {
    if (strcmp_local(cmd, "help") == 0) {
        terminal_write("AngOS Built-in Commands:\n");
        terminal_write("  help     - Show available commands\n");
        terminal_write("  clear    - Clear terminal screen\n");
        terminal_write("  ver      - Display OS kernel version\n");
        terminal_write("  reboot   - Reboot system\n");
    } else if (strcmp_local(cmd, "clear") == 0) {
        terminal_clear();
    } else if (strcmp_local(cmd, "ver") == 0) {
        terminal_write("AngOS v0.1.0 (x86_64 Architecture)\n");
    } else if (strcmp_local(cmd, "reboot") == 0) {
        terminal_write("Rebooting system...\n");
        outb(0x64, 0xFE); // Pulse CPU reset line via 8042 controller
    } else if (cmd[0] != '\0') {
        terminal_write("Unknown command. Type 'help' for commands.\n");
    }
}

void kernel_main(void) {
    char input_buf[128];

    terminal_write("\n=== AngOS Command Shell ===\n");
    terminal_write("Type 'help' to get started.\n\n");

    while (1) {
        terminal_write("AngOS> ");
        keyboard_gets(input_buf, sizeof(input_buf));
        execute_command(input_buf);
    }
}