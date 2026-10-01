#include <stddef.h>
#include <types.h>

void *memset(void *dest, int value, size_t count) {
    unsigned char *ptr = dest;

    while (count--)
        *ptr++ = (unsigned char)value;

    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    unsigned char *d = dest;
    const unsigned char *s = src;

    while (count--)
        *d++ = *s++;

    return dest;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }

    return (unsigned char)*a - (unsigned char)*b;
}
