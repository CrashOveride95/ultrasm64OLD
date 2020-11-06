#ifndef STRING_H
#define STRING_H

#include "PR/ultratypes.h"

void *memcpy(void *dst, const void *src, size_t size);
void    *memset(void* dst, int c, size_t count);
size_t strlen(const char *str);
char *strchr(const char *str, s32 ch);
char  *strcat(char *str1, const char *str2);

#endif
