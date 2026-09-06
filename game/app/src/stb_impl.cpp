// Single translation unit for the stb_image implementation (tile loader).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
// Apple clang 17 deprecates sprintf, which stb_image_write still uses.
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <stb_image_write.h>
#pragma GCC diagnostic pop
