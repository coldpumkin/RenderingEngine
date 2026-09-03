// stb_image keeps its declarations and its implementation in one header, and the
// implementation is emitted only where STB_IMAGE_IMPLEMENTATION is defined. This file
// is that one translation unit -- the same shape as cgltf.c and vma.cpp.
//
// .c rather than .cpp because stb is plain C, which is how upstream tests it.
//
// The decoders we do not use are compiled out. Sponza is jpg and png only, and every
// other format is dead code that still costs build time and binary size. Adding one
// back is a line here, and the failure if we forget is loud: stbi_load returns null
// with "unknown image type" rather than doing something subtle.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
//
// stdio stays on, so stbi_load takes a path. Reading the bytes ourselves would mean
// another file reader beside Shader.cpp's ReadSpirv, which cannot be shared -- it
// returns uint32_t and insists the size divides by four. Two readers that differ is
// worse than letting stb open its own file.
//
// ASCII only: the directory clears COMPILE_OPTIONS to drop the /W4 we impose on our
// own code, and that takes /utf-8 with it.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
