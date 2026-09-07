# Third party

Dependencies are vendored under `Source/ThirdParty/`, one directory each, with a
`VERSION.md` recording the upstream URL, the files taken, and how to update them. Vendoring
keeps configure working without a network, pins the version in the commit, and keeps the
sources readable in place. Upstream CMake files are not used; each directory has a minimal
one, and `/W4` inheritance is cut there so third-party warnings stay out of the build.

| Library | Used for | License | What is in this repository |
|---|---|---|---|
| [volk](https://github.com/zeux/volk) | Vulkan function pointer loader | MIT | sources and `LICENSE.md` |
| [Dear ImGui](https://github.com/ocornut/imgui) | debug panel | MIT | sources and `LICENSE.txt` |
| [GLM](https://github.com/g-truc/glm) | math | MIT (Happy Bunny) | headers and `LICENSE.txt` |
| [cgltf](https://github.com/jkuhlmann/cgltf) | glTF parsing | MIT, notice at the end of the file | `cgltf.h` |
| [stb_image](https://github.com/nothings/stb) | image decoding | public domain / MIT, notice at the end of the file | `stb_image.h` |
| [SPIRV-Reflect](https://github.com/KhronosGroup/SPIRV-Reflect) | shader reflection | Apache-2.0, notice in each file | sources |
| [GLFW](https://github.com/glfw/glfw) | window and input | Zlib | **not redistributed**; fetched by CMake during configure |
| [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU memory allocation | MIT | **not redistributed**; the header comes from the Vulkan SDK, only `vma.cpp` is here |

GLFW is the one exception to vendoring. volk is two files whose loading mechanism is worth
reading; GLFW is dozens of files whose internals are not the point of this project. The
cost is that the first configure needs a network, after which it is cached.

VMA ships declarations and implementation in one 20,000-line header, so `vma.cpp` is the
single translation unit that defines `VMA_IMPLEMENTATION`. It sets both
`VMA_STATIC_VULKAN_FUNCTIONS` and `VMA_DYNAMIC_VULKAN_FUNCTIONS` to 0 and fills
`VmaVulkanFunctions` by hand, so that VMA calls through the same device table volk loaded
rather than resolving its own pointers.
