#pragma once
void *memset(void *d, int c, unsigned long long n);
void *memcpy(void *d, const void *s, unsigned long long n);
void *memmove(void *d, const void *s, unsigned long long n);
int memcmp(const void *a, const void *b, unsigned long long n);
char *strcpy(char *d, const char *s);
char *strcat(char *d, const char *s);
unsigned long long strlen(const char *s);
