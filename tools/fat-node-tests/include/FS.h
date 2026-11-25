#ifndef ANGOS_TEST_FS_H
#define ANGOS_TEST_FS_H
#include <types.h>
#include <disk.h>
#include <stdio.h>
#define FILENAME_LENGTH 256

#define FMKDIR_OK 1
#define FMKDIR_NO_SPACE 0
#define FMKDIR_INVALID -1
#define FMKDIR_IO_ERROR -2
#define FMKDIR_EXISTS -3

extern FILE *files;

int fmkdir(char *path);
#endif
