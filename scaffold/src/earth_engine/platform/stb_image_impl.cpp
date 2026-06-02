// stb_image 单一定义单元（避免多重定义链接错误）
#if __has_include(<stb_image.h>)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#endif
