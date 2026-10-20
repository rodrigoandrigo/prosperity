#pragma once
#include <sys/stat.h>
int prosperity_stat(const char*, struct stat*);
// Function-like macro intentionally leaves `struct stat` unchanged.
#define stat(...) prosperity_stat(__VA_ARGS__)
