// Call the real DLL's internal System V syscall entry points. The script
// resolves section-relative symbol offsets from this exact build's COFF table.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include "kern/lv2/sys_time.h"
#include "kern/lv2/error_table.h"
#define REQUIRE(x) do { if(!(x)) { std::printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(int argc, char** argv) {
  REQUIRE(argc==5);
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  AddDllDirectory(std::filesystem::u8path(argv[1]).c_str());
  AddDllDirectory(std::filesystem::u8path(argv[2]).c_str());
  auto dll=LoadLibraryExW((std::filesystem::u8path(argv[1])/L"prosperity.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  REQUIRE(dll);
  auto* image=reinterpret_cast<unsigned char*>(dll);
  auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(image+reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
  auto* text=IMAGE_FIRST_SECTION(nt);
  REQUIRE(std::memcmp(text->Name,".text",5)==0);
  auto local=reinterpret_cast<decltype(&kern::sys_localtime_to_utc)>(image+text->VirtualAddress+std::strtoull(argv[3],nullptr,16));
  auto utc=reinterpret_cast<decltype(&kern::sys_utc_to_localtime)>(image+text->VirtualAddress+std::strtoull(argv[4],nullptr,16));
  struct Output { i64 time; kern::sce_time_conversion info; i32 dst; i32 guard; };
  for(i64 time : {i64(0),i64(-1),i64(1729429200),i64(2208988800),std::numeric_limits<i64>::max()}) {
    for(i64 selector : {i64(-1),i64(0),i64(1)}) {
      Output out{-7,{-7,-7,-7},-7,0x12345678};
      REQUIRE(local(time,selector,&out.time,&out.info,&out.dst)==0);
      REQUIRE(out.time==time && out.info.time==time && out.info.utc_offset_seconds==0 && out.info.dst_offset_seconds==0 && out.dst==0 && out.guard==0x12345678);
      REQUIRE(utc(out.time,&out.time,&out.info,&out.dst)==0 && out.time==time);
    }
  }
  REQUIRE(local(123,1,nullptr,nullptr,nullptr)==0);
  REQUIRE(utc(123,nullptr,nullptr,nullptr)==0);
  REQUIRE(local(123,1,reinterpret_cast<i64*>(1),nullptr,nullptr)==-int(kern::SysError::eFAULT));
  REQUIRE(local(123,1,nullptr,reinterpret_cast<kern::sce_time_conversion*>(1),nullptr)==-int(kern::SysError::eFAULT));
  REQUIRE(local(123,1,nullptr,nullptr,reinterpret_cast<i32*>(1))==-int(kern::SysError::eFAULT));
  void* page=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  REQUIRE(page);
  DWORD old; REQUIRE(VirtualProtect(page,4096,PAGE_READONLY,&old));
  REQUIRE(utc(123,static_cast<i64*>(page),nullptr,nullptr)==-int(kern::SysError::eFAULT));
  VirtualFree(page,0,MEM_RELEASE);
  std::puts("PASS real DLL syscall 638/639: DST selector 1, 64-bit timestamps, complete outputs, optional outputs, invalid/read-only pointers");
  return 0;
}
