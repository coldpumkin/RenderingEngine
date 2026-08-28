#include "Engine.h"

#include <cstdio>

using namespace LambdaEngine;

int main() {
    auto engine = Engine::Create(1280, 720, "Lambda Engine");
    if (engine == nullptr) {
        return 1;
    }

    DynamicRHI& rhi = engine->RHI();

    // **뷰포트는 그리려는 쪽이 만든다.** Engine은 뷰포트 없이도 유효하므로 그 생성 조건이
    // 아니다. engine보다 뒤에 선언해야 먼저 죽는다 (서피스가 창보다 먼저 죽어야 한다).
    auto viewport = rhi.CreateViewport(engine->WindowHandle());
    if (viewport == nullptr) {
        return 1;
    }

    std::fprintf(stderr, "close the window to exit.\n");

    while (engine->PumpEvents()) {
        // 창 크기 변화는 Engine이 받아두고 여기서 가져간다 - Engine은 뷰포트를 모른다.
        if (engine->ConsumeResized()) {
            rhi.ResizeViewport(*viewport);
        }

        // nullptr은 실패가 아니다. 최소화 중이면 그릴 곳이 없다.
        RHICommandContext* context = rhi.BeginDrawingViewport(*viewport);
        if (context == nullptr) {
            continue;
        }

        context->BeginRenderPass(*viewport);

        // ---- 드로우콜이 들어올 자리 ----

        context->EndRenderPass();

        rhi.EndDrawingViewport(*viewport);
    }

    return 0;
}
