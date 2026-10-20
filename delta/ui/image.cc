#if (defined(__linux__) && !defined(__ANDROID__)) || defined(DELTA_UWP)
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif
