//
// DirectXPage.xaml.cpp
// Implementação da classe DirectXPage.
//

#include "pch.h"
#include "StartupLog.h"
#include "DirectXPage.xaml.h"
#include <ppltasks.h>

using namespace Prosperity_UWP;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Graphics::Display;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace concurrency;

DirectXPage::DirectXPage():
	m_windowVisible(true)
{
	StartupLog::Write(L"DirectXPage InitializeComponent");
    InitializeComponent();
    StartupLog::Write(L"DirectXPage XAML loaded");
    m_xbox = Windows::System::Profile::AnalyticsInfo::VersionInfo->DeviceFamily == "Windows.Xbox";
    if(m_xbox) {
        try { InitializePointerPages(); }
        catch (Platform::Exception^ error) {
            m_pointerFailed = true;
            StartupLog::Error(L"Xbox pointer pages unavailable; continuing startup", error);
        }
        StartupLog::Write(L"Register Xbox BackRequested handler");
        try {
            m_backToken = SystemNavigationManager::GetForCurrentView()->BackRequested +=
                ref new EventHandler<BackRequestedEventArgs^>([](Object^, BackRequestedEventArgs^ args) { args->Handled = true; });
            m_backRegistered = true;
        } catch (Platform::Exception^ error) { StartupLog::Error(L"Xbox BackRequested unavailable; continuing startup", error); }
    }

	// Registre manipuladores de eventos para o ciclo de vida da página.
	CoreWindow^ window = Window::Current->CoreWindow;

	window->VisibilityChanged +=
		ref new TypedEventHandler<CoreWindow^, VisibilityChangedEventArgs^>(this, &DirectXPage::OnVisibilityChanged);

	DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();

	currentDisplayInformation->DpiChanged +=
		ref new TypedEventHandler<DisplayInformation^, Object^>(this, &DirectXPage::OnDpiChanged);

	currentDisplayInformation->OrientationChanged +=
		ref new TypedEventHandler<DisplayInformation^, Object^>(this, &DirectXPage::OnOrientationChanged);

	DisplayInformation::DisplayContentsInvalidated +=
		ref new TypedEventHandler<DisplayInformation^, Object^>(this, &DirectXPage::OnDisplayContentsInvalidated);

	swapChainPanel->CompositionScaleChanged += 
		ref new TypedEventHandler<SwapChainPanel^, Object^>(this, &DirectXPage::OnCompositionScaleChanged);

	swapChainPanel->SizeChanged +=
		ref new SizeChangedEventHandler(this, &DirectXPage::OnSwapChainPanelSizeChanged);

	// Neste ponto, temos acesso ao dispositivo. 
	// É possível criar recursos dependentes do dispositivo.
	StartupLog::Write(L"Create graphics devices");
    m_deviceResources = std::make_shared<DX::DeviceResources>();
	m_deviceResources->SetSwapChainPanel(swapChainPanel);

	StartupLog::Write(L"Initialize emulator host");
    m_main = std::unique_ptr<Prosperity_UWPMain>(new Prosperity_UWPMain(m_deviceResources));
	coreStatus->Text = m_main->Host().Status();
    m_statusTimer = ref new DispatcherTimer();
    TimeSpan interval; interval.Duration = 2500000;
    m_statusTimer->Interval = interval;
    Platform::WeakReference weakThis(this);
    m_statusTimer->Tick += ref new EventHandler<Object^>([weakThis](Object^, Object^) {
        auto page = weakThis.Resolve<DirectXPage>();
        if (page && page->m_main) {
            page->UpdateSystemPointer();
            page->coreStatus->Text = page->m_main->Host().Status();
            page->coreStatus->Visibility = page->m_main->Host().FrontendAvailable() ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
            const auto actions = page->m_main->Host().TakeActions();
            if (actions & 3) page->OpenPicker((actions & 2) != 0);
            if (actions & (32 | 64)) page->OpenPicker(false, true);
            if (actions & 4) Application::Current->Exit();
        }
    });
    auto weakPage = Platform::WeakReference(this);
    auto pointer = ref new PointerEventHandler([weakPage](Object^, PointerRoutedEventArgs^ args) {
        auto page = weakPage.Resolve<DirectXPage>();
        if (!page || !page->m_main) return;
        const auto point = args->GetCurrentPoint(page->swapChainPanel);
        prosperity_input e{}; e.type = 1; e.x = point->Position.X; e.y = point->Position.Y;
        page->m_main->Host().Input(e);
        const bool buttons[] = {point->Properties->IsLeftButtonPressed, point->Properties->IsRightButtonPressed,
            point->Properties->IsMiddleButtonPressed, point->Properties->IsXButton1Pressed, point->Properties->IsXButton2Pressed};
        for (unsigned i = 0; i < 5; ++i) { e = {}; e.type = 2; e.code = i; e.x = buttons[i] ? 1.0f : 0.0f; page->m_main->Host().Input(e); }
        args->Handled = true;
    });
    swapChainPanel->PointerMoved += pointer;
    swapChainPanel->PointerPressed += ref new PointerEventHandler([weakPage, pointer](Object^ sender, PointerRoutedEventArgs^ args) {
        auto page = weakPage.Resolve<DirectXPage>();
        if (page) page->swapChainPanel->CapturePointer(args->Pointer);
        pointer(sender, args);
    });
    swapChainPanel->PointerReleased += ref new PointerEventHandler([weakPage, pointer](Object^ sender, PointerRoutedEventArgs^ args) {
        pointer(sender, args);
        auto page = weakPage.Resolve<DirectXPage>();
        if (page) page->swapChainPanel->ReleasePointerCapture(args->Pointer);
    });
    swapChainPanel->PointerCaptureLost += ref new PointerEventHandler([weakPage](Object^, PointerRoutedEventArgs^) {
        auto page = weakPage.Resolve<DirectXPage>(); if (!page || !page->m_main) return;
        for (unsigned i = 0; i < 5; ++i) { prosperity_input e{}; e.type = 2; e.code = i; page->m_main->Host().Input(e); }
    });
    swapChainPanel->PointerWheelChanged += ref new PointerEventHandler([weakPage](Object^, PointerRoutedEventArgs^ args) {
        auto page = weakPage.Resolve<DirectXPage>(); if (!page || !page->m_main) return;
        auto point = args->GetCurrentPoint(page->swapChainPanel);
        prosperity_input e{}; e.type = 3;
        if (point->Properties->IsHorizontalMouseWheel) e.x = point->Properties->MouseWheelDelta / 120.0f;
        else e.y = point->Properties->MouseWheelDelta / 120.0f;
        page->m_main->Host().Input(e); args->Handled = true;
    });
    window->KeyDown += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>([weakPage](CoreWindow^, KeyEventArgs^ args) {
        auto page = weakPage.Resolve<DirectXPage>(); if (!page || !page->m_main) return;
        const auto key=static_cast<uint32_t>(args->VirtualKey);
        if(page->m_xbox && (key==27 || (key>=195 && key<=218))) { args->Handled=true; return; }
        prosperity_input e{}; e.type = 4; e.code = key;
        if (e.code == 16) e.code = args->KeyStatus.ScanCode == 0x36 ? 161 : 160;
        if (e.code == 17) e.code = args->KeyStatus.IsExtendedKey ? 163 : 162;
        if (e.code == 18) e.code = args->KeyStatus.IsExtendedKey ? 165 : 164; e.x = 1;
        page->m_main->Host().Input(e); args->Handled = true;
    });
    window->KeyUp += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>([weakPage](CoreWindow^, KeyEventArgs^ args) {
        auto page = weakPage.Resolve<DirectXPage>(); if (!page || !page->m_main) return;
        const auto key=static_cast<uint32_t>(args->VirtualKey);
        if(page->m_xbox && (key==27 || (key>=195 && key<=218))) { args->Handled=true; return; }
        prosperity_input e{}; e.type = 4; e.code = key;
        if (e.code == 16) e.code = args->KeyStatus.ScanCode == 0x36 ? 161 : 160;
        if (e.code == 17) e.code = args->KeyStatus.IsExtendedKey ? 163 : 162;
        if (e.code == 18) e.code = args->KeyStatus.IsExtendedKey ? 165 : 164;
        page->m_main->Host().Input(e); args->Handled = true;
    });
    window->CharacterReceived += ref new TypedEventHandler<CoreWindow^, CharacterReceivedEventArgs^>([weakPage](CoreWindow^, CharacterReceivedEventArgs^ args) {
        auto page = weakPage.Resolve<DirectXPage>(); if (!page || !page->m_main) return;
        prosperity_input e{}; e.type = 5; e.code = args->KeyCode; page->m_main->Host().Input(e); args->Handled = true;
    });
    window->Activated += ref new TypedEventHandler<CoreWindow^, WindowActivatedEventArgs^>([weakPage](CoreWindow^, WindowActivatedEventArgs^ args) {
        auto page = weakPage.Resolve<DirectXPage>(); if (!page || !page->m_main) return;
        prosperity_input e{}; e.type = 0; e.code = args->WindowActivationState != CoreWindowActivationState::Deactivated;
        page->m_main->Host().Input(e);
    });
    m_statusTimer->Start();
    std::vector<task<void>> grants;
    auto access = Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList;
    auto storageHost = m_main->SharedHost();
    for (auto entry : access->Entries) {
        auto token = entry.Token;
        grants.push_back(create_task(access->GetItemAsync(token)).then([storageHost, token](task<Windows::Storage::IStorageItem^> item) {
            try { storageHost->RegisterStorage(item.get(), token); } catch (...) {}
        }));
    }
    when_all(grants.begin(), grants.end()).then([weakThis]() {
        auto page = weakThis.Resolve<DirectXPage>();
        if (page && page->m_main) { page->m_grantsReady = true; if (page->m_pendingTitle) page->LaunchTitle(page->m_pendingTitle); if (page->m_windowVisible) page->m_main->StartRenderLoop(); }
    }, task_continuation_context::use_current());
}

DirectXPage::~DirectXPage()
{
    if(m_backRegistered) {
        try { SystemNavigationManager::GetForCurrentView()->BackRequested -= m_backToken; }
        catch (Platform::Exception^ error) { StartupLog::Error(L"Unregister Xbox BackRequested", error); }
    }
    if (m_statusTimer) m_statusTimer->Stop();
	// Interrompa a renderização e o processamento de eventos em destruição.
	m_main->StopRenderLoop();

}

// Salva o estado atual do aplicativo para eventos de suspensão e de encerramento.
void DirectXPage::SaveInternalState(IPropertySet^ state)
{
    if (!m_main) return;
    prosperity_input input{}; input.type = 0; m_main->Host().Input(input);
    m_main->StopRenderLoop();
    m_main->Host().Pause(true);
    critical_section::scoped_lock lock(m_main->GetCriticalSection());
    m_deviceResources->Trim();
}
// Carrega o estado atual do aplicativo para eventos de retomada.
void DirectXPage::LoadInternalState(IPropertySet^ state)
{
	prosperity_input input{}; input.type = 0; input.code = 1; m_main->Host().Input(input);
	m_main->Host().Pause(false);
	// Coloque aqui o código para carregar o estado do aplicativo.

	// Inicie a renderização quando o aplicativo for retomado.
	m_main->StartRenderLoop();
}

// Manipuladores de eventos da janela.

void DirectXPage::OnVisibilityChanged(CoreWindow^ sender, VisibilityChangedEventArgs^ args)
{
	m_windowVisible = args->Visible;
    prosperity_input input{}; input.type = 0; input.code = m_windowVisible ? 1 : 0; m_main->Host().Input(input);
	if (m_windowVisible)
	{
		if (m_grantsReady) m_main->StartRenderLoop();
	}
	else
	{
		m_main->StopRenderLoop();
	}
}

// Manipuladores de eventos DisplayInformation.

void DirectXPage::OnDpiChanged(DisplayInformation^ sender, Object^ args)
{
	if (!m_main) return;
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	// Observação: o valor de LogicalDpi recuperado aqui pode não corresponder à DPI efetiva do aplicativo
	// se ele está sendo escalado para dispositivos de alta resolução. Depois que o DPI é definido em DeviceResources,
	// você sempre deve recuperá-la usando o método GetDpi.
	// Consulte DeviceResources.cpp para obter mais detalhes.
	m_deviceResources->SetDpi(sender->LogicalDpi);

}

void DirectXPage::OnOrientationChanged(DisplayInformation^ sender, Object^ args)
{
	if (!m_main) return;
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	m_deviceResources->SetCurrentOrientation(sender->CurrentOrientation);

}

void DirectXPage::OnDisplayContentsInvalidated(DisplayInformation^ sender, Object^ args)
{
	if (!m_main) return;
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	m_deviceResources->ValidateDevice();
}

void DirectXPage::OnCompositionScaleChanged(SwapChainPanel^ sender, Object^ args)
{
	if (!m_main) return;
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	m_deviceResources->SetCompositionScale(sender->CompositionScaleX, sender->CompositionScaleY);

}

void DirectXPage::OnSwapChainPanelSizeChanged(Object^ sender, SizeChangedEventArgs^ e)
{
	if (!m_main) return;
	critical_section::scoped_lock lock(m_main->GetCriticalSection());
	m_deviceResources->SetLogicalSize(e->NewSize);

}



void DirectXPage::InitializePointerPages() {
    // Windows evaluates RequiresPointer when a Page gains focus. Keep two
    // distinct pages and transfer the existing surface; retain the swapchain.
    StartupLog::Write(L"Create Xbox menu and gameplay pointer pages");
    auto menu = ref new Page();
    menu->RequiresPointer = Windows::UI::Xaml::Controls::RequiresPointer::WhenFocused;
    auto game = ref new Page();
    game->RequiresPointer = Windows::UI::Xaml::Controls::RequiresPointer::Never;
    menu->IsTabStop = true;
    game->IsTabStop = true;
    auto focus = ref new RoutedEventHandler([](Object^ sender, RoutedEventArgs^) {
        try {
            if (!safe_cast<Page^>(sender)->Focus(Windows::UI::Xaml::FocusState::Programmatic))
                StartupLog::Write(L"Xbox pointer page focus pending");
        } catch (Platform::Exception^ error) { StartupLog::Error(L"Xbox pointer page focus unavailable", error); }
    });
    menu->Loaded += focus;
    game->Loaded += focus;
    auto frame = ref new Windows::UI::Xaml::Controls::Frame();
    RequiresPointer = Windows::UI::Xaml::Controls::RequiresPointer::Never;
    auto surface = Content;
    Content = nullptr;
    try {
        menu->Content = surface;
        frame->Content = menu;
        Content = frame;
    } catch (...) {
        menu->Content = nullptr;
        Content = surface;
        throw;
    }
    m_menuPointerPage = menu;
    m_gamePointerPage = game;
    m_pointerFrame = frame;
    m_pointerSurface = surface;
    StartupLog::Write(L"Xbox pointer pages ready");
}

void DirectXPage::UpdateSystemPointer() {
    if(!m_xbox || !m_main || m_pointerFailed || !m_pointerFrame) return;
    const bool hidden = m_main->Host().HideSystemPointer();
    if(hidden == m_pointerHidden) return;
    SwitchPointerPage(hidden);
}

void DirectXPage::SwitchPointerPage(bool hidden) {
    auto previous = m_pointerHidden ? m_gamePointerPage : m_menuPointerPage;
    auto next = hidden ? m_gamePointerPage : m_menuPointerPage;
    try {
        previous->Content = nullptr;
        next->Content = m_pointerSurface;
        m_pointerFrame->Content = next;
        m_pointerHidden = hidden;
        next->Focus(Windows::UI::Xaml::FocusState::Programmatic);
        StartupLog::Write(hidden ? L"Xbox system pointer disabled for gameplay" : L"Xbox system pointer restored for frontend");
    } catch (Platform::Exception^ error) {
        m_pointerFailed = true;
        StartupLog::Error(L"Xbox pointer transition unavailable; preserving renderer", error);
        try {
            next->Content = nullptr;
            previous->Content = m_pointerSurface;
            m_pointerFrame->Content = previous;
        } catch (Platform::Exception^ restoreError) { StartupLog::Error(L"Restore Xbox pointer surface", restoreError); }
    }
}

void DirectXPage::ValidatePointerPages() {
    // Explicit diagnostic activation also exercises page reparenting on PC.
    // This does not claim to emulate the console's OS cursor behavior.
    try {
        if(!m_pointerFrame) InitializePointerPages();
        auto panel = swapChainPanel;
        for(int i=0;i<4;++i) {
            SwitchPointerPage((i % 2) == 0);
            UpdateLayout();
            if(m_pointerFailed || swapChainPanel != panel ||
               m_pointerFrame->Content != (m_pointerHidden ? m_gamePointerPage : m_menuPointerPage))
                throw ref new FailureException(L"Pointer page round trip failed");
        }
        StartupLog::Write(L"PASS pointer page round trips; SwapChainPanel retained");
    } catch(Platform::Exception^ error) { StartupLog::Error(L"FAIL pointer page validation",error); }
}

void DirectXPage::OpenPicker(bool folder, bool modules)
{
    auto weakPage = Platform::WeakReference(this);
    auto complete = [weakPage](task<Windows::Storage::IStorageItem^> result) {
        auto page = weakPage.Resolve<DirectXPage>(); if (!page || !page->m_main) return;
        try {
            auto item = result.get();
            Platform::String^ mountedPath = nullptr;
            if (item) {
                auto access = Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList;
                // Retain the broker object; a raw path alone does not grant access.
                uint64_t hash = 14695981039346656037ull;
                for (const wchar_t* c = item->Path->Data(); *c; ++c) {
                    hash = (hash ^ (*c & 255)) * 1099511628211ull;
                    hash = (hash ^ (*c >> 8)) * 1099511628211ull;
                }
                auto token = ref new Platform::String((L"prosperity-" + std::to_wstring(hash)).c_str());
                if (item->Path->IsEmpty()) token = access->Add(item);
                else access->AddOrReplace(token, item);
                mountedPath = page->m_main->Host().RegisterStorage(item, token);
            }
            page->m_main->Host().PickerCompleted(mountedPath);
        } catch (...) { page->m_main->Host().PickerCompleted(nullptr); }
    };
    if (folder) {
        auto picker = ref new Windows::Storage::Pickers::FolderPicker();
        picker->SuggestedStartLocation = Windows::Storage::Pickers::PickerLocationId::ComputerFolder;
        picker->FileTypeFilter->Append("*");
        create_task(picker->PickSingleFolderAsync()).then([](Windows::Storage::StorageFolder^ item) {
            return static_cast<Windows::Storage::IStorageItem^>(item);
        }).then(complete, task_continuation_context::use_current());
    } else {
        auto picker = ref new Windows::Storage::Pickers::FileOpenPicker();
        picker->SuggestedStartLocation = Windows::Storage::Pickers::PickerLocationId::ComputerFolder;
        if(modules) { picker->FileTypeFilter->Append(".rar"); picker->FileTypeFilter->Append(".zip"); }
        else for (auto extension : {L".pkg", L".ffpkg", L".zip", L".rar", L".bin", L".elf"})
            picker->FileTypeFilter->Append(ref new String(extension));
        create_task(picker->PickSingleFileAsync()).then([](Windows::Storage::StorageFile^ item) {
            return static_cast<Windows::Storage::IStorageItem^>(item);
        }).then(complete, task_continuation_context::use_current());
    }
}

void DirectXPage::LaunchTitle(Platform::String^ path) {
    m_pendingTitle = path;
    if (!m_grantsReady || !m_main || !path || path->IsEmpty()) return;
    StartupLog::Write(L"Launch requested title after restoring brokered grants");
    m_main->Host().Boot(path);
    m_pendingTitle = nullptr;
}
