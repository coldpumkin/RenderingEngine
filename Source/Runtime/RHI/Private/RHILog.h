#pragma once

#include <cstdio>

// RHI 모듈의 임시 로깅. Vulkan 전용이 아니라서 Private/ 바로 밑에 둔다.
//
// 존재 이유는 교체 지점을 하나로 줄이는 것이다. 지금은 라이브러리인 RHI가 stderr에
// 직접 쓰고 있는데, 출력 정책은 애플리케이션이 정할 일이다. Application이 로거를
// 소유하게 되면 이 파일만 바꾸면 된다.
//
// stdout이 아니라 stderr인 이유: stdout은 파이프로 리다이렉트되면 블록 버퍼링이 되어
// 비정상 종료 시 버퍼가 통째로 사라진다.
//
// 함수가 아니라 매크로인 이유: 포맷 문자열이 fprintf 호출부까지 리터럴로 도달해야
// 컴파일러가 인자를 대조할 수 있다.
//
// __VA_OPT__는 C++20 기능이고 MSVC에서는 /Zc:preprocessor가 필요하다.

#define LAMBDA_LOG_INFO(format, ...) \
    std::fprintf(stderr, "[RHI] " format "\n" __VA_OPT__(,) __VA_ARGS__)

#define LAMBDA_LOG_ERROR(format, ...) \
    std::fprintf(stderr, "[RHI] " format "\n" __VA_OPT__(,) __VA_ARGS__)
