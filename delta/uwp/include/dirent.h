#pragma once
#include <stdint.h>
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent { uint64_t d_ino; unsigned char d_type; char d_name[4096]; };
struct prosperity_DIR;
typedef struct prosperity_DIR DIR;
extern "C" {
DIR* opendir(const char*);
struct dirent* readdir(DIR*);
int closedir(DIR*);
}
