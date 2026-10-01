#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include <types.h>
#include <stddef.h>

void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
int strcmp(const char *a, const char *b);

#endif