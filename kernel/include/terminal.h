#ifndef ANGOS_TERMINAL_H
#define ANGOS_TERMINAL_H

void terminal_init(void);
void terminal_putchar(char c);
void terminal_write(const char *str);
void terminal_clear(void);

#endif
