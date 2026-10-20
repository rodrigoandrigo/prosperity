#include "pch.h"
#include "StartupLog.h"
#include "Prosperity_UWPMain.h"
#include <chrono>

using namespace Prosperity_UWP;
using namespace Windows::Foundation;
using namespace Windows::System::Threading;
using namespace Concurrency;

Prosperity_UWPMain::Prosperity_UWPMain(const std::shared_ptr<DX::DeviceResources>& resources)
    : m_deviceResources(resources), m_host(std::make_shared<ProsperityHost>()),
      m_state(std::make_shared<RenderState>())
{
    m_host->Initialize();
}

Prosperity_UWPMain::~Prosperity_UWPMain()
{
    StopRenderLoop();
}

void Prosperity_UWPMain::StartRenderLoop()
{
    critical_section::scoped_lock lock(m_state->mutex);
    if (m_state->running)
        return;
    m_state->running = true;
    auto state = m_state;
    auto resources = m_deviceResources;
    auto host = m_host;
    auto handler = ref new WorkItemHandler([state, resources, host](IAsyncAction^ action)
    {
        StartupLog::Write(L"Render loop started");
        bool firstFrame = true;
        try {
        auto previous = std::chrono::steady_clock::now();
        while (action->Status == AsyncStatus::Started)
        {
            critical_section::scoped_lock lock(state->mutex);
            if (!state->running || action->Status != AsyncStatus::Started)
                break;
            auto context = resources->GetD3DDeviceContext();
            auto viewport = resources->GetScreenViewport();
            context->RSSetViewports(1, &viewport);
            ID3D11RenderTargetView* targets[] = { resources->GetBackBufferRenderTargetView() };
            context->OMSetRenderTargets(1, targets, nullptr);
            context->ClearRenderTargetView(targets[0], DirectX::Colors::Black);
            context->OMSetRenderTargets(0, nullptr, nullptr);
            host->Draw(resources->GetD2DDeviceContext());
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - previous).count(); previous = now;
            const auto logical = resources->GetLogicalSize();
            host->RenderFrontend(resources->GetD3DDevice(), context, targets[0],
                static_cast<uint32_t>(viewport.Width), static_cast<uint32_t>(viewport.Height),
                logical.Width, logical.Height, dt);
            resources->Present(host->ImmediatePresentation() ? 0 : 1);
            if (firstFrame) { StartupLog::Write(L"First frame presented"); firstFrame = false; }
        }
        } catch (Platform::Exception^ e) { StartupLog::Error(L"FATAL render loop", e); throw; }
        catch (const std::exception& e) { StartupLog::Write(L"FATAL render loop C++ exception"); OutputDebugStringA(e.what()); throw; }
        catch (...) { StartupLog::Write(L"FATAL render loop unknown exception"); throw; }
        StartupLog::Write(L"Render loop stopped");
    });
    m_renderLoopWorker = ThreadPool::RunAsync(handler, WorkItemPriority::High, WorkItemOptions::TimeSliced);
}

void Prosperity_UWPMain::StopRenderLoop()
{
    critical_section::scoped_lock lock(m_state->mutex);
    m_state->running = false;
    if (m_renderLoopWorker != nullptr)
        m_renderLoopWorker->Cancel();
}
