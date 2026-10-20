#pragma once
#include <filesystem>
#include <windows.h>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include "formats/archive_backend.h"
#include "io/file.h"
#include "io/path.h"
namespace uwp::frontend {
// Runs on a worker. No ImGui state or input mailbox is accessed here.
inline std::string ImportModules(const std::string& source, bool ps5) {
  namespace fs = std::filesystem;
  const auto root = fs::u8path(io::MakeAbsPath(base::String(ps5 ? "modules/ps5" : "modules/ps4")).c_str());
  const auto stage = root.parent_path() / (ps5 ? "ps5.importing" : "ps4.importing");
  const auto backup = root.parent_path() / (ps5 ? "ps5.backup" : "ps4.backup");
  auto move=[](const fs::path& from,const fs::path& to) {
    if(!MoveFileFromAppW(from.c_str(),to.c_str())) throw std::system_error(GetLastError(),std::system_category(),"Cannot move module directory");
  };
  bool staged=false;
  try {
    if (fs::exists(stage) || fs::exists(backup)) throw std::runtime_error("An interrupted import needs recovery; preserve the .importing/.backup folders.");
    auto archive = formats::OpenZipBackend(base::String(source.c_str()));
    if (!archive) archive = formats::OpenRarBackend(base::String(source.c_str()));
    base::Vector<formats::ArchiveEntry> entries;
    if (!archive || !archive->Index(entries)) throw std::runtime_error("Cannot read ZIP/RAR archive (encrypted or corrupt archives are unsupported).");
    std::set<std::string> names;
    std::vector<size_t> selected;
    uint64_t total = 0;
    for (size_t i=0; i<entries.size(); ++i) {
      std::string name(entries[i].path.c_str());
      std::replace(name.begin(), name.end(), '\\', '/');
      if (name.empty() || name.front()=='/' || name.find(':')!=std::string::npos || name.find('\0')!=std::string::npos) throw std::runtime_error("Unsafe archive path.");
      auto relative = fs::u8path(name);
      for (const auto& part : relative) if (part=="..") throw std::runtime_error("Unsafe archive path.");
      name = relative.filename().string();
      std::string key=name; std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
      auto ext=fs::path(key).extension().string();
      if(ext!=".sprx" && ext!=".prx" && ext!=".elf" && ext!=".dll") continue;
      if(name.back()=='.' || name.back()==' ' || !names.insert(key).second) throw std::runtime_error("Duplicate or invalid module filename.");
      if(entries[i].size > (512ull<<20) || (total+=entries[i].size) > (4ull<<30) || selected.size()>=8192) throw std::runtime_error("Module archive exceeds import limits.");
      selected.push_back(i);
    }
    if(selected.empty()) throw std::runtime_error("No .sprx, .prx, .elf or .dll modules found.");
    fs::create_directories(root.parent_path());
    staged=true;
    if(fs::exists(root)) fs::copy(root,stage,fs::copy_options::recursive); else fs::create_directory(stage);
    std::vector<unsigned char> buffer(1<<20);
    for(auto i:selected) {
      const auto& entry=entries[i];
      std::string name(entry.path.c_str()); std::replace(name.begin(),name.end(),'\\','/');
      const auto out=stage/fs::u8path(name).filename();
      auto utf8=out.u8string();
      io::File file(base::String(reinterpret_cast<const char*>(utf8.c_str())),io::FileMode::kTrunc);
      if(!file.IsOpen()) throw std::runtime_error("Cannot create module file.");
      for(uint64_t offset=0;offset<entry.size;) {
        auto count=std::min<uint64_t>(buffer.size(),entry.size-offset);
        auto got=archive->ExtractRange(entry,buffer.data(),offset,count);
        if(got<=0 || static_cast<uint64_t>(got)>count || file.Write(buffer.data(),got)!=static_cast<uint64_t>(got)) throw std::runtime_error("Module extraction failed (corrupt archive or storage full).");
        offset+=got;
      }
      file.Flush(); file.Close();
    }
    bool old=fs::exists(root);
    if(old) move(root,backup);
    try { move(stage,root); } catch(...) { if(old) move(backup,root); throw; }
    std::error_code cleanup; if(old) fs::remove_all(backup,cleanup);
    return "Imported " + std::to_string(selected.size()) + (ps5 ? " PS5 modules." : " PS4 modules.");
  } catch(const std::exception& e) {
    std::error_code cleanup;
    // Only remove staging created by this invocation; existing recovery data stays intact.
    if(staged && !fs::exists(backup,cleanup)) fs::remove_all(stage,cleanup);
    return std::string("Import failed: ")+e.what();
  }
}
}
