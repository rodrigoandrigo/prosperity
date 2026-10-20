//
// DirectXPage.xaml.h
// Declaração da classe DirectXPage.
//

#pragma once

#include "DirectXPage.g.h"

#include "Common\DeviceResources.h"
#include "Prosperity_UWPMain.h"

namespace Prosperity_UWP
{
	/// <summary>
	/// Uma página que hospeda um SwapChainPanel do DirectX.
	/// </summary>
	public ref class DirectXPage sealed
	{
	public:
		DirectXPage();
		virtual ~DirectXPage();
        void LaunchTitle(Platform::String^ path);

		void SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state);

	private:
        void OpenPicker(bool folder, bool modules = false);
		// Manipulador de eventos de renderização de baixo nível XAML.


		// Manipuladores de eventos da janela.
		void OnVisibilityChanged(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::VisibilityChangedEventArgs^ args);

		// Manipuladores de eventos DisplayInformation.
		void OnDpiChanged(Windows::Graphics::Display::DisplayInformation^ sender, Platform::Object^ args);
		void OnOrientationChanged(Windows::Graphics::Display::DisplayInformation^ sender, Platform::Object^ args);
		void OnDisplayContentsInvalidated(Windows::Graphics::Display::DisplayInformation^ sender, Platform::Object^ args);

		// Outros manipuladores de eventos.

		void OnCompositionScaleChanged(Windows::UI::Xaml::Controls::SwapChainPanel^ sender, Object^ args);
		void OnSwapChainPanelSizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);

		// Rastreie nossa entrada independente em um thread de trabalho de segundo plano.



		// Funções de manipulação de entrada independente.




		// Recursos usados para renderizar o conteúdo do DirectX na tela de fundo da página XAML.
		std::shared_ptr<DX::DeviceResources> m_deviceResources;
		std::unique_ptr<Prosperity_UWPMain> m_main; 
		bool m_windowVisible;
        bool m_grantsReady = false;
        Platform::String^ m_pendingTitle;
        Windows::UI::Xaml::DispatcherTimer^ m_statusTimer;
	};
}


