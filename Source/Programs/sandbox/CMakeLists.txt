# **One pair per concept.** The same axis Unreal's VulkanRHI uses: a new resource adds
# a new pair rather than growing an existing file. No abstraction layer -- structs and
# free functions.
#
# Three layers, and the arrows only point one way. **No header under Vulkan/ includes
# anything above it**, and that is the whole of the rule:
#
#   main.cpp  ->  Renderer  ->  Gui . Passes . Vertex  ->  Vulkan/*
#
# Inside Vulkan/ it runs one way too:
#   Core <- Instance <- Device <- { Swapchain, Commands, Buffer, Image, Shader }
#                                    ^                              ^
#                                  Window                      Pipeline, Descriptors
#                          (the surface is instance level,
#                           and the window holds the swapchain)
add_executable(sandbox
    Config.h                # compile-time knobs

    Vulkan/Core.h           # the two function tables, what we require, the RAII rules
    Vulkan/Instance.h    Vulkan/Instance.cpp
    Vulkan/Device.h      Vulkan/Device.cpp      # + queue families, queues, GPU choice, VMA
    Vulkan/Window.h      Vulkan/Window.cpp      # window system, window, surface
    Vulkan/Swapchain.h   Vulkan/Swapchain.cpp
    Vulkan/Image.h       Vulkan/Image.cpp       # image + view, shared by targets and textures
    Vulkan/Attachments.h Vulkan/Attachments.cpp # the spec of what a pass draws into
    Vulkan/Texture.h     Vulkan/Texture.cpp     # an image a shader reads
    Vulkan/Barrier.h     Vulkan/Barrier.cpp     # image layout transitions
    Vulkan/Descriptors.h Vulkan/Descriptors.cpp # how a shader reaches an image or buffer
    Vulkan/Commands.h    Vulkan/Commands.cpp    # command pools, one per queue family
    Vulkan/Frame.h       Vulkan/Frame.cpp       # frame slot, frame target, opening and closing
    Vulkan/Shader.h      Vulkan/Shader.cpp      # reading .spv, and reflecting on it
    Vulkan/Pipeline.h    Vulkan/Pipeline.cpp
    Vulkan/VertexLayout.h                       # what a vertex buffer hands the vertex stage
    Vulkan/Buffer.h      Vulkan/Buffer.cpp
    Vulkan/Mesh.h        Vulkan/Mesh.cpp        # the two buffers a draw always binds together

    Vertex.h             Vertex.cpp        # our vertex type and the layout for it
    Passes.h             Passes.cpp        # what the passes share, and the frame that orders them
    ShadowPass.h         ShadowPass.cpp    # depth only, from where the light is
    ScenePass.h          ScenePass.cpp     # the surfaces, off-screen and multisampled
    PostProcessPass.h    PostProcessPass.cpp # the scene's resolve onto the frame's target
    Gui.h                Gui.cpp           # one ImGui panel, and the pass that draws it
    Renderer.h                             # everything that needs a device, grouped by kind

    main.cpp                # what is created, and what a frame is handed
)

# Its own folder as an include root, so "Vulkan/Device.h" says which group a header is
# in at the point of inclusion.
target_include_directories(sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# While this is written flat, sandbox links volk and GLFW directly. There is no module
# in between, so there is nothing to hide behind one.
target_link_libraries(sandbox PRIVATE volk glfw vma glm spirv-reflect cgltf stb imgui)

# std::fopen is C4996 (deprecated) under MSVC. fopen_s is not standard, so taking it
# would make these files MSVC-only -- and what is read here is a shader file and an
# asset, where nothing is gained.
target_compile_definitions(sandbox PRIVATE _CRT_SECURE_NO_WARNINGS)

# Assets are not copied next to the executable the way shaders are. A shader is a build
# output and belongs in the build tree; an asset is source, and Sponza alone is 9.3 MB
# before its textures bring it past 50 MB. The source path is compiled in instead.
#
# The path not existing is normal -- assets are gitignored, so each machine fetches its
# own, and main says so and exits.
target_compile_definitions(sandbox PRIVATE
    LAMBDA_ASSET_ROOT="${CMAKE_SOURCE_DIR}/Assets")

# Validation on or off. Always defined as 0 or 1, so a typo in an #if cannot pass
# silently the way an undefined name would.
if (LAMBDA_ENABLE_VULKAN_VALIDATION)
    target_compile_definitions(sandbox PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=1)
else()
    target_compile_definitions(sandbox PRIVATE LAMBDA_ENABLE_VULKAN_VALIDATION=0)
endif()

# ---- shaders: GLSL -> SPIR-V ----
#
# **Compiled at build time**, not at run time. glslc ships with the Vulkan SDK, so this
# adds no dependency. Runtime compilation (linking shaderc or glslang) is worth it when
# shaders are edited while the program runs, and nothing here does that.
find_program(GLSLC_EXE glslc HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)

# **OUTPUT cannot take a generator expression** like $<TARGET_FILE_DIR:...>: at
# configure time the target does not exist yet, so CMake has nothing to evaluate. The
# .spv files are written to a fixed path and copied next to the executable in
# POST_BUILD, where expressions are allowed.
set(SHADER_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/Shaders")
set(SPV_FILES "")

foreach(SHADER shadow.vert shadow.frag scene.vert scene.frag fullscreen.vert post.frag gui.vert gui.frag)
    set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/${SHADER}")
    set(SPV "${SHADER_BUILD_DIR}/${SHADER}.spv")
    add_custom_command(
        OUTPUT  "${SPV}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_BUILD_DIR}"
        COMMAND "${GLSLC_EXE}" "${SRC}" -o "${SPV}"
        DEPENDS "${SRC}"
        COMMENT "glslc ${SHADER}"
        VERBATIM
    )
    list(APPEND SPV_FILES "${SPV}")
endforeach()

# A target of its own that sandbox depends on, so editing only a shader still rebuilds
# what has to be rebuilt.
add_custom_target(sandbox_shaders DEPENDS ${SPV_FILES})
add_dependencies(sandbox sandbox_shaders)

# The executable looks for "Shaders/..." relative to itself, so they have to be there.
add_custom_command(TARGET sandbox POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${SHADER_BUILD_DIR}" "$<TARGET_FILE_DIR:sandbox>/Shaders"
    COMMENT "copying shaders next to sandbox.exe"
    VERBATIM
)

# So the sources show up in VS. HEADER_FILE_ONLY on all of them, not just some: without
# it VS treats a listed .vert as something to compile itself, and the rule above is
# already what compiles them.
set(SHADER_SOURCES
    Shaders/shadow.vert     Shaders/shadow.frag
    Shaders/scene.vert      Shaders/scene.frag
    Shaders/fullscreen.vert Shaders/post.frag
    Shaders/gui.vert        Shaders/gui.frag
)
target_sources(sandbox PRIVATE ${SHADER_SOURCES})
set_source_files_properties(${SHADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

set_target_properties(sandbox sandbox_shaders PROPERTIES FOLDER "Engine/Programs")
