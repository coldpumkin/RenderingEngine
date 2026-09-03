// cgltf keeps its declarations and its implementation in one header, and the
// implementation is emitted only where CGLTF_IMPLEMENTATION is defined. This file is
// that one translation unit -- the same shape as vma.cpp, and for the same reason:
// putting the implementation inside our own code would mix a third party's build time
// and warnings into a file we maintain.
//
// .c rather than .cpp because cgltf is plain C, so this is how upstream tests it.
// volk.c is here for the same reason, which is why the top-level project() enables C.
//
// ASCII only: the directory clears COMPILE_OPTIONS to drop the /W4 we impose on our
// own code, and that takes /utf-8 with it.

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
