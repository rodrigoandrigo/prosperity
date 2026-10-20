#include "base/environment_variables.h"
#include <map>
#include <mutex>
#include <string>

namespace base {
namespace {
struct Environment {
  std::mutex mutex;
  std::map<std::u8string, std::u8string> values;
};
Environment& State() { static Environment state; return state; }
std::u8string Key(StringRefU8 text) { return {text.data(), static_cast<size_t>(text.size())}; }
}
bool GetEnvironmentVariable(StringRefU8 name, StringU8& output) {
  auto& state = State();
  std::lock_guard lock(state.mutex);
  const auto found = state.values.find(Key(name));
  if (found == state.values.end()) return false;
  output = StringU8(found->second.data(), found->second.size());
  return true;
}
bool SetEnvironmentVariable(StringRefU8 name, StringRefU8 value) {
  if (name.empty()) return false;
  auto& state = State();
  std::lock_guard lock(state.mutex);
  state.values[Key(name)] = Key(value);
  return true;
}
bool DeleteEnvironmentVariable(StringRefU8 name) {
  auto& state = State();
  std::lock_guard lock(state.mutex);
  return state.values.erase(Key(name)) != 0;
}
}
