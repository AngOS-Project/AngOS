#include <keyboard.h>
#include <cpu/IO.h>
#include <terminal.h>

extern void piceoi(bool isslave);

static bool shift_pressed = false;
static bool caps_lock = false;

static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint16_t kbd_head = 0;
static volatile uint16_t kbd_tail = 0;

static const char scancode_map[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
     0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
     0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
   '*',   0, ' '
};

static const char scancode_map_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
     0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
     0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
   '*',   0, ' '
};

void keyboard_init(void) {
    uint8_t mask = inb(0x21);
    outb(0x21, mask & ~(1 << 1)); // Unmask IRQ1
}

static void kbd_push(char c) {
    uint16_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) {
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
    }
}

bool keyboard_has_char(void) {
    return kbd_head != kbd_tail;
}

char keyboard_getchar(void) {
    while (!keyboard_has_char()) {
        __asm__ volatile("hlt");
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

void keyboard_gets(char *buf, size_t max_len) {
    size_t i = 0;
    while (i < max_len - 1) {
        char c = keyboard_getchar();
        if (c == '\n') {
            terminal_putchar('\n');
            break;
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                terminal_putchar('\b');
            }
        } else if (c >= ' ' && c <= '~') {
            buf[i++] = c;
            terminal_putchar(c);
        }
    }
    buf[i] = '\0';
}

void keyboard_handler(void) {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            shift_pressed = false;
        }
    } else {
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = true;
        } else if (scancode == 0x3A) {
            caps_lock = !caps_lock;
        } else if (scancode < sizeof(scancode_map)) {
            bool use_shift = shift_pressed ^ caps_lock;
            char ch = use_shift ? scancode_map_shift[scancode] : scancode_map[scancode];
            if (ch != 0) {
                kbd_push(ch);
            }
        }
    }
    piceoi(false);
}

__attribute__((interrupt)) void irq1_stub(void *frame) {
    (void)frame;
    keyboard_handler();
}