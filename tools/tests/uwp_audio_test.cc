// Production backend, real XAudio2 voices/device, no UWP application or guest.
#include "../../delta/host/audio_output_xaudio2.cc"
#include <thread>
#include <cstdio>
#include <stdexcept>
namespace base { void WriteStandardError(const char* text, mem_size length) noexcept { std::fwrite(text, 1, length, stderr); } }
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
int main() {
  try {
    Check(host::OpenAudioPort(0, 2, 0) == -1 && host::OpenAudioPort(48000, 3, 0) == -1, "bad format");
    for (const int floating : {0, 1}) for (const unsigned channels : {1u, 2u, 8u}) {
      const int h = host::OpenAudioPort(48000, channels, floating);
      Check(h >= 0, "real XAudio2 output device/voice required");
      auto port = host::Find(h);
      Check(port && port->voice, "actual output device required for playback test");
      host::SetAudioPortVolume(h, .01f);
      float volume = 0; port->voice->GetVolume(&volume); Check(std::abs(volume - .01f) < .0001f, "volume");
      const int frames = 480;
      std::vector<float> f32(frames * channels);
      std::vector<int16_t> s16(frames * channels);
      for (int i = 0; i < frames; ++i) for (unsigned c = 0; c < channels; ++c) {
        const float value = .02f * std::sin(6.2831853f * 440 * i / 48000);
        f32[i * channels + c] = value; s16[i * channels + c] = int16_t(value * 32767);
      }
      const void* pcm = floating ? static_cast<const void*>(f32.data()) : s16.data();
      for (int i = 0; i < 12; ++i) Check(host::QueueAudio(h, pcm, frames) == frames, "submit PCM");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (port->completed.load() < 12 * frames && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      Check(port->completed == 12 * frames && port->queued == 0, "device consumed exact frame count");
      host::SetAudioPaused(true);
      const auto before = port->submitted.load();
      Check(host::QueueAudio(h, pcm, frames) == frames && port->submitted == before, "pause cannot accumulate audio");
      host::SetAudioPaused(false);
      Check(host::QueueAudio(h, pcm, frames) == frames, "resume");
      host::CloseAudioPort(h); Check(host::QueueAudio(h, pcm, frames) == -1, "closed handle");
      for (auto& slot : port->slots) Check(!slot.busy, "slot lifetime after DestroyVoice");
      printf("PASS %uch %s, real playback callbacks, gain, pause/resume/close\n", channels, floating ? "F32" : "S16");
    }
    const int h = host::OpenAudioPort(44100, 2, 0);
    Check(h >= 0, "44.1 kHz resampling");
    std::atomic<bool> running{true};
    std::thread producer([&] { int16_t silence[512]{}; while (running) host::QueueAudio(h, silence, 256); });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    host::CloseAllAudioPorts(); running = false; producer.join();
    Check(host::QueueAudio(h, nullptr, 0) == -1, "concurrent close");
    const int restarted = host::OpenAudioPort(48000, 2, 0);
    Check(restarted >= 0, "engine recreated");
    auto port = host::Find(restarted);
    Check(port && port->voice, "recreated voice");
    auto old = port->owner;
    old->error = XAUDIO2_E_DEVICE_INVALIDATED;
    int16_t silence[512]{};
    Check(host::QueueAudio(restarted, silence, 256) == 256 && port->owner != old, "critical engine error recovers through a new engine");
    host::CloseAllAudioPorts();
    puts("PASS 44100 Hz, concurrent submission/close, engine recreation and injected critical error recovery"); return 0;
  } catch (const std::exception& e) { fprintf(stderr, "FAIL %s\n", e.what()); host::CloseAllAudioPorts(); return 1; }
}
