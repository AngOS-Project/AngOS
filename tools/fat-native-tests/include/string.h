#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H
#include <stddef.h>
void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
#endif
