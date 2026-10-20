#pragma once
#include <cstdint>
namespace uwp {
int SignalAction(int signal, const void* action, void* previous);
int SignalMask(int how, const void* mask, void* previous);
int SendThreadSignal(uint32_t tid, int signal);
int SuspendForSignal(const void* mask);
bool HasPendingSignal();
void PollSignals();
}
