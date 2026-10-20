#pragma once
#include "Common\DeviceResources.h"
#include "ProsperityHost.h"

namespace Prosperity_UWP
{
    class Prosperity_UWPMain
    {
    public:
        explicit Prosperity_UWPMain(const std::shared_ptr<DX::DeviceResources>& deviceResources);
        ~Prosperity_UWPMain();
        void StartRenderLoop();
        void StopRenderLoop();
        std::shared_ptr<ProsperityHost> SharedHost() { return m_host; }
        ProsperityHost& Host() { return *m_host; }
        Concurrency::critical_section& GetCriticalSection() { return m_state->mutex; }
    private:
        struct RenderState {
            Concurrency::critical_section mutex;
            bool running = false;
        };
        std::shared_ptr<DX::DeviceResources> m_deviceResources;
        std::shared_ptr<ProsperityHost> m_host;
        std::shared_ptr<RenderState> m_state;
        Windows::Foundation::IAsyncAction^ m_renderLoopWorker;
    };
}
