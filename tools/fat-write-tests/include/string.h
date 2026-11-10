#ifndef ANGOS_FAT_WRITE_TEST_STRING_H
#define ANGOS_FAT_WRITE_TEST_STRING_H
#include <stddef.h>
void *memset(void *dest, int value, size_t count);
void *memcpy(void *dest, const void *src, size_t count);
int memcmp(const void *a, const void *b, size_t count);
int strcmp(const char *a, const char *b);
#endif
