#include "host/audio_output.h"
#include "base/standard_streams.h"
#include <windows.h>
#include <xaudio2.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace host {
namespace {
struct Engine final : IXAudio2EngineCallback {
  IXAudio2* audio = nullptr;
  IXAudio2MasteringVoice* master = nullptr;
  std::atomic<HRESULT> error{S_OK};
  ~Engine() {
    if (audio) audio->UnregisterForCallbacks(this);
    if (master) master->DestroyVoice();
    if (audio) audio->Release();
  }
  bool Init() {
    const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto result = XAudio2Create(&audio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(result)) result = audio->RegisterForCallbacks(this);
    if (SUCCEEDED(result)) result = audio->CreateMasteringVoice(&master, XAUDIO2_DEFAULT_CHANNELS,
        48000, 0, nullptr, nullptr, AudioCategory_GameEffects);
    if (SUCCEEDED(apartment)) CoUninitialize();
    error = result;
    if (FAILED(result)) {
      char message[100]; const int length = std::snprintf(message, sizeof(message), "[audio] XAudio2 initialization failed: 0x%08lx\n", static_cast<unsigned long>(result));
      base::WriteStandardError(message, length);
    }
    return SUCCEEDED(result);
  }
  void STDMETHODCALLTYPE OnProcessingPassStart() override {}
  void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnCriticalError(HRESULT value) override { error = value; }
};
struct Slot {
  std::vector<unsigned char> data;
  std::atomic<bool> busy{false};
  uint32_t frames = 0;
};
std::mutex registryMutex;
std::shared_ptr<Engine> engine;
std::atomic<bool> paused{false};
std::chrono::steady_clock::time_point nextEngineAttempt{};
std::shared_ptr<Engine> GetEngine() {
  std::lock_guard lock(registryMutex);
  if (!engine || FAILED(engine->error.load())) {
    const auto now = std::chrono::steady_clock::now();
    if (now < nextEngineAttempt) return {};
    nextEngineAttempt = now + std::chrono::milliseconds(250);
    auto replacement = std::make_shared<Engine>();
    if (!replacement->Init()) return {};
    engine = std::move(replacement);
    nextEngineAttempt = {};
  }
  return engine;
}
struct Port final : IXAudio2VoiceCallback {
  std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<Engine> owner;
  IXAudio2SourceVoice* voice = nullptr;
  WAVEFORMATEXTENSIBLE format{};
  std::array<Slot, 32> slots;
  std::atomic<uint32_t> queued{0};
  std::atomic<uint64_t> submitted{0}, completed{0};
  std::atomic<HRESULT> error{S_OK};
  float gain = 1;
  bool closed = false;
  bool running = false;
  ~Port() { Close(); }
  void Destroy() {
    if (voice) { voice->DestroyVoice(); voice = nullptr; }
    // DestroyVoice waits for callbacks; slot memory remains owned until here.
    for (auto& slot : slots) slot.busy = false;
    queued = 0;
    running = false;
  }
  void Close() {
    std::lock_guard lock(mutex); closed = true; Destroy(); owner.reset(); changed.notify_all();
  }
  bool EnsureVoice() {
    if (closed) return false;
    if (voice && SUCCEEDED(error.load()) && SUCCEEDED(owner->error.load())) return ApplyPause();
    Destroy(); owner = GetEngine();
    if (!owner) return false;
    error = owner->audio->CreateSourceVoice(&voice, &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
    if (FAILED(error.load())) return false;
    voice->SetVolume(gain);
    ApplyPause();
    return SUCCEEDED(error.load());
  }
  bool ApplyPause() {
    const bool desired = !paused.load();
    if (voice && desired != running) {
      error = desired ? voice->Start() : voice->Stop();
      if (SUCCEEDED(error.load())) running = desired;
    }
    return SUCCEEDED(error.load());
  }
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT value) override { error = value; changed.notify_all(); }
  void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
    auto* slot = static_cast<Slot*>(context);
    const auto frames = slot->frames;
    queued.fetch_sub(frames); completed.fetch_add(frames);
    slot->busy.store(false, std::memory_order_release); changed.notify_all();
  }
};
std::map<int, std::shared_ptr<Port>> ports;
int nextHandle = 0;
std::shared_ptr<Port> Find(int handle) {
  std::lock_guard lock(registryMutex);
  const auto it = ports.find(handle); return it == ports.end() ? nullptr : it->second;
}
}
int OpenAudioPort(u32 freq, u32 channels, int isFloat) {
  if (freq < XAUDIO2_MIN_SAMPLE_RATE || freq > XAUDIO2_MAX_SAMPLE_RATE ||
      (channels != 1 && channels != 2 && channels != 8) || (isFloat != 0 && isFloat != 1)) return -1;
  try {
    auto port = std::make_shared<Port>();
    auto& f = port->format; f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = WORD(channels); f.Format.nSamplesPerSec = freq;
    f.Format.wBitsPerSample = isFloat ? 32 : 16;
    f.Format.nBlockAlign = WORD(channels * f.Format.wBitsPerSample / 8);
    f.Format.nAvgBytesPerSec = freq * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = f.Format.wBitsPerSample;
    f.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : channels == 2 ?
        SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : KSAUDIO_SPEAKER_7POINT1_SURROUND;
    // Standard WAVE subtype GUID, avoiding a dependency on the KS user library.
    f.SubFormat = {DWORD(isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM), 0, 0x10,
        {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
    // Retain a logical port even if no output device currently exists. The
    // daemon opens only when its format changes; subsequent PCM submissions
    // must therefore be able to recover when an output device returns.
    port->EnsureVoice();
    std::lock_guard lock(registryMutex);
    if (nextHandle == INT_MAX) return -1;
    const int handle = nextHandle++; ports.emplace(handle, std::move(port)); return handle;
  } catch (...) { return -1; }
}
int QueueAudio(int handle, const void* samples, u32 frames) {
  auto port = Find(handle); if (!port || (frames && !samples) || frames > INT_MAX) return -1;
  try {
    std::unique_lock lock(port->mutex);
    if (!port->EnsureVoice()) return -1;
    if (paused || !frames) return int(frames);
    const uint32_t rate = port->format.Format.nSamplesPerSec;
    const auto align = port->format.Format.nBlockAlign;
    uint32_t offset = 0;
    while (offset < frames) {
      if (port->closed || FAILED(port->error.load())) return -1;
      if (paused) return int(frames);
      // Audio needs a bounded live queue. Wait only on the submitting worker;
      // callbacks never take this mutex, allocate, or wait for the producer.
      if (port->queued.load() >= rate / 10) {
        const bool ready = port->changed.wait_for(lock, std::chrono::milliseconds(20), [&] {
          return port->closed || paused || FAILED(port->error.load()) || port->queued.load() < rate / 10;
        });
        if (!ready) return int(offset); // A stalled device cannot hang the guest.
        continue;
      }
      Slot* slot = nullptr;
      for (auto& candidate : port->slots) if (!candidate.busy.load(std::memory_order_acquire)) { slot = &candidate; break; }
      if (!slot) return int(offset);
      const uint32_t count = (std::min)(frames - offset, (std::max)(1u, rate / 100));
      slot->data.resize(size_t(count) * align);
      std::memcpy(slot->data.data(), static_cast<const unsigned char*>(samples) + size_t(offset) * align, slot->data.size());
      slot->frames = count; slot->busy = true; port->queued.fetch_add(count);
      XAUDIO2_BUFFER buffer{}; buffer.AudioBytes = UINT32(slot->data.size());
      buffer.pAudioData = slot->data.data(); buffer.pContext = slot;
      const auto result = port->voice->SubmitSourceBuffer(&buffer);
      if (FAILED(result)) { port->queued.fetch_sub(count); slot->busy = false; port->error = result; return -1; }
      port->submitted.fetch_add(count); offset += count;
    }
    return int(frames);
  } catch (...) { return -1; }
}
void SetAudioPortVolume(int handle, float gain) {
  auto port = Find(handle); if (!port) return;
  std::lock_guard lock(port->mutex);
  port->gain = std::isfinite(gain) ? std::clamp(gain, 0.f, 1.f) : 0.f;
  if (port->voice) port->voice->SetVolume(port->gain);
}
void CloseAudioPort(int handle) {
  std::shared_ptr<Port> port;
  { std::lock_guard lock(registryMutex); auto it = ports.find(handle); if (it == ports.end()) return; port = std::move(it->second); ports.erase(it); }
  port->Close();
}
void CloseAllAudioPorts() {
  std::map<int, std::shared_ptr<Port>> retired;
  { std::lock_guard lock(registryMutex); retired.swap(ports); }
  for (auto& entry : retired) entry.second->Close();
  { std::lock_guard lock(registryMutex); if (ports.empty()) engine.reset(); }
  paused = false;
}
void SetAudioPaused(bool value) {
  if (paused.exchange(value) == value) return;
  std::vector<std::shared_ptr<Port>> snapshot;
  { std::lock_guard lock(registryMutex); for (auto& entry : ports) snapshot.push_back(entry.second); }
  for (auto& port : snapshot) {
    std::lock_guard lock(port->mutex);
    if (port->voice && !port->closed) {
      port->ApplyPause();
    }
    port->changed.notify_all();
  }
}
} // namespace host
