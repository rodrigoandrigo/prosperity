#pragma once
#include <windows.h>
#include <cstdio>
#include <exception>
namespace StartupLog {
inline HANDLE& File() { static HANDLE h = INVALID_HANDLE_VALUE; return h; }
inline void Write(const wchar_t* message) noexcept {
    if (File() == INVALID_HANDLE_VALUE) return;
    wchar_t line[4096]; SYSTEMTIME t; GetSystemTime(&t);
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%04u-%02u-%02uT%02u:%02u:%02uZ thread=%lu %s\r\n", t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentThreadId(),message);
    char bytes[16384]; int n=WideCharToMultiByte(CP_UTF8,0,line,-1,bytes,sizeof(bytes),nullptr,nullptr);
    DWORD written; if(n>1) { WriteFile(File(),bytes,n-1,&written,nullptr); FlushFileBuffers(File()); }
}
inline void Error(const wchar_t* stage, Platform::Exception^ e) noexcept {
    wchar_t text[3500]; _snwprintf_s(text,_countof(text),_TRUNCATE,L"%s HRESULT=0x%08X %s",stage,e->HResult,e->Message->Data()); Write(text);
}
inline LONG WINAPI Crash(EXCEPTION_POINTERS* e) {
    wchar_t text[256]; _snwprintf_s(text,_countof(text),_TRUNCATE,L"FATAL native exception=0x%08X address=%p",e->ExceptionRecord->ExceptionCode,e->ExceptionRecord->ExceptionAddress); Write(text); return EXCEPTION_CONTINUE_SEARCH;
}
inline void Initialize() noexcept {
    try {
        auto path=Windows::Storage::ApplicationData::Current->LocalFolder->Path;
        auto name=path+L"\\startup.log";
        File()=CreateFile2(name->Data(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,OPEN_ALWAYS,nullptr);
        Write(L"SESSION BEGIN (previous final stage retained above)");
        SetUnhandledExceptionFilter(Crash);
        std::set_terminate([] { Write(L"FATAL std::terminate"); RaiseFailFastException(nullptr,nullptr,0); });
    } catch (...) { OutputDebugStringW(L"Prosperity startup log initialization failed\n"); }
}
}
