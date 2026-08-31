# GLM - 행렬/벡터 수학. 헤더 온리라 컴파일할 것이 없다.
#
# volk처럼 벤더링했고 GLFW처럼 FetchContent로 가지 않았다. 판단 기준은 셋:
#
#   내부가 학습 대상인가   **그렇다.** ext/matrix_clip_space.inl에 perspectiveRH_ZO와
#                          perspectiveRH_NO가 나란히 있고, 그 둘의 차이가 이 프로젝트가
#                          지금 배우는 것(Vulkan 깊이 [0,1])과 정확히 같은 주제다.
#   파일이 몇 개인가       302개. GLFW보다 많지만 **빌드하지 않는다** - 헤더 온리라
#                          configure도 컴파일도 늘지 않는다. GLFW를 FetchContent로 보낸
#                          비용(빌드 시간)이 여기엔 없다.
#   네트워크 없이 되는가   된다. 이게 벤더링의 원래 이유다.
#
# 업스트림 CMakeLists를 안 쓰고 이 최소 버전을 쓴다 (volk와 같은 이유).

# 컴파일할 소스가 없어 INTERFACE다. 경고 정책 상속을 끊을 대상도 없다 -
# GLM 헤더는 이 디렉터리가 아니라 **그것을 include한 쪽의 번역 단위**에서 컴파일된다.
add_library(glm INTERFACE)

# SYSTEM을 안 붙인다. Spikes/GlmCheck가 /W4로 빌드해서 경고 0을 확인했으므로 숨길 게
# 없고, 나중에 업스트림을 올렸을 때 새 경고가 조용히 묻히지 않는 편이 낫다.
target_include_directories(glm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# **이 정의가 이 타깃에 붙어 있는 것이 요점이다.** 소비자가 깜빡할 수 없게 한다.
#
# 없으면 glm::perspective가 OpenGL 규약([-1,1])으로 깊이를 만든다. Vulkan은 [0,1]이라
# 가까운 절반이 통째로 잘려 나가는데, 검증 레이어도 컴파일러도 아무 말을 안 한다.
# 화면만 이상해지는 종류의 실수다.
#
# 확인: Spikes/GlmCheck가 near/far를 실제로 변환해서 0.000/1.000이 나오는 것을 찍는다.
target_compile_definitions(glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE)

# INTERFACE 라이브러리는 VS 솔루션에 안 보인다. 헤더를 트리에서 열어보려면 타깃이
# 있어야 해서 빈 커스텀 타깃 하나를 둔다 - 빌드에는 아무 영향이 없다.
# (302개를 전부 나열하지 않는다. 실제로 include하는 것만 적는다.)
add_custom_target(glm_headers SOURCES
    VERSION.md
    glm/glm.hpp
    glm/ext/matrix_clip_space.hpp   # perspective. ZO와 NO가 갈리는 곳
    glm/ext/matrix_transform.hpp    # translate · rotate · scale · lookAt
    glm/gtc/type_ptr.hpp            # value_ptr - push constant로 memcpy할 때
)
set_target_properties(glm_headers PROPERTIES FOLDER "ThirdParty")
