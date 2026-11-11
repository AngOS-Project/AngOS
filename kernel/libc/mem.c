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

int memcmp(const void *a, const void *b, size_t count) {
    const unsigned char *x = a;
    const unsigned char *y = b;

    while (count--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        ++x;
        ++y;
    }

    return 0;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }

    return (unsigned char)*a - (unsigned char)*b;
}
