# UWP startup diagnostics

The native host appends UTF-8 records to `ApplicationData.Current.LocalFolder/startup.log` before loading App XAML. Each line includes UTC time and thread ID and is flushed with FlushFileBuffers. Previous sessions remain in the same file.

Records cover page navigation, XAML loading, device/host creation, window activation, first presentation, suspension, unhandled XAML errors (message and HRESULT), native unhandled exceptions (code and address), std::terminate, and render worker exceptions. Fatal errors are logged and retain their normal failure behavior. OS forced termination or power loss cannot run an exception handler; the last flushed stage is retained.

For the current package identity on PC, the file is under `%LOCALAPPDATA%/Packages/22a83681-cd4c-45e5-a17f-65c9f91345d9_txmtwkwvzcfp4/LocalState/startup.log`. The core separately maintains its existing prosperity.log.

## Startup failure corrected

Installed version 1.0.0.1 reproduced the failure and captured: `Setting 'Background' property is not supported on SwapChainPanel`, HRESULT `0x802B000A`. Removed Background from SwapChainPanel in DirectXPage.xaml. The render target is already cleared by the native renderer.

Release x64 1.0.0.2 was built, signed using Prosperity-UWP_TemporaryKey.pfx, installed as an update, and activated successfully on PC. The log reached `First frame presented`; the process remained responsive and no new Prosperity Application Error event was observed during the check. This validates startup on PC, not game emulation or Xbox operation.
