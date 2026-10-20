"""Standalone embedding ABI test: no package installation or game execution."""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--runtime', type=Path, default=Path('Prosperity-UWP/Runtime/Release'))
parser.add_argument('--vclibs', type=Path, required=True)
args = parser.parse_args()
runtime = args.runtime.resolve()
directories = [os.add_dll_directory(str(p.resolve())) for p in [runtime, args.vclibs]]
library = c.CDLL(str(runtime / 'prosperity.dll'))
kernel = c.WinDLL('kernel32')
kernel.SetUnhandledExceptionFilter.argtypes = [c.c_void_p]
kernel.SetUnhandledExceptionFilter.restype = c.c_void_p
filter_type = c.WINFUNCTYPE(c.c_long, c.c_void_p)
host_filter = filter_type(lambda unused: 0)
host_filter_pointer = c.cast(host_filter, c.c_void_p).value
original_filter = kernel.SetUnhandledExceptionFilter(host_filter_pointer)
sdl = c.CDLL(str(runtime / 'SDL3.dll'))
sdl.SDL_GetLogOutputFunction.argtypes = [c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
original_log, original_log_user = c.c_void_p(), c.c_void_p()
sdl.SDL_GetLogOutputFunction(c.byref(original_log), c.byref(original_log_user))
frame_type = c.CFUNCTYPE(None, c.c_void_p, c.c_void_p, c.c_uint32, c.c_uint32, c.c_uint32, c.c_uint32)
log_type = c.CFUNCTYPE(None, c.c_void_p, c.c_uint32, c.c_char_p)
state_type = c.CFUNCTYPE(None, c.c_void_p, c.c_int, c.c_int, c.c_char_p)
class Config(c.Structure):
    _fields_ = [('size', c.c_uint32), ('version', c.c_uint32), ('directory', c.c_char_p),
                ('user', c.c_void_p), ('frame', frame_type), ('log', log_type), ('state', state_type)]
library.prosperity_get_abi_version.restype = c.c_uint32
library.prosperity_create.argtypes = [c.POINTER(Config), c.POINTER(c.c_void_p)]
library.prosperity_boot.argtypes = [c.c_void_p, c.c_char_p]
library.prosperity_pause.argtypes = [c.c_void_p, c.c_uint32]
library.prosperity_stop.argtypes = [c.c_void_p]
library.prosperity_request_stop.argtypes = [c.c_void_p]
library.prosperity_wait.argtypes = [c.c_void_p, c.c_uint32]
library.prosperity_get_state.argtypes = [c.c_void_p]
library.prosperity_destroy.argtypes = [c.c_void_p]
library.prosperity_get_last_error.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32]
library.prosperity_get_last_error.restype = None
assert library.prosperity_get_abi_version() == 2
logs, states, reentry = [], [], []
context = c.c_void_p()
ready_entered, release_ready = threading.Event(), threading.Event()
block_ready = False
@frame_type
def frame(*unused): pass
@log_type
def log(user, level, message):
    logs.append(message.decode('utf-8'))
    if context.value: reentry.append(library.prosperity_wait(context, 0))
@state_type
def state(user, current, result, message):
    states.append((current, result, message.decode('utf-8')))
    if context.value:
        library.prosperity_get_state(context)
        reentry.append(library.prosperity_wait(context, 0))
        reentry.append(library.prosperity_destroy(context))
    if current == 0 and block_ready:
        ready_entered.set()
        release_ready.wait(5)
data_path = Path('build/uwp-platform-tests/abi-data').resolve()
data = str(data_path).encode('utf-8')
config = Config(c.sizeof(Config), 2, data, None, frame, log, state)
assert library.prosperity_create(None, c.byref(context)) == -1
config.version = 1
assert library.prosperity_create(c.byref(config), c.byref(context)) == -1
config.version = 2
for cycle in range(2):
    context = c.c_void_p()
    assert library.prosperity_create(c.byref(config), c.byref(context)) == 0
    assert library.prosperity_wait(context, 5000) == 0
    duplicate = c.c_void_p()
    assert library.prosperity_create(c.byref(config), c.byref(duplicate)) == -2
    assert library.prosperity_pause(context, 1) == -2
    assert library.prosperity_boot(context, b'') == -1
    started = time.monotonic()
    assert library.prosperity_boot(context, data + b'/absent-title') == 0
    assert time.monotonic() - started < 0.5
    assert library.prosperity_wait(context, 5000) == -3
    assert library.prosperity_get_state(context) == 6
    assert library.prosperity_boot(context, bytes([255])) == 0
    # Broker-aware stat reports an unreadable/invalid path as I/O failure;
    # it no longer relies on std::filesystem throwing during UTF-8 conversion.
    assert library.prosperity_wait(context, 5000) == -3
    message = c.create_string_buffer(2048)
    library.prosperity_get_last_error(context, message, len(message))
    assert message.value
    tiny = c.create_string_buffer(1)
    library.prosperity_get_last_error(context, tiny, 1)
    assert tiny.raw == b'\0'
    assert library.prosperity_stop(context) == 0
    assert library.prosperity_destroy(context) == 0
    assert kernel.SetUnhandledExceptionFilter(host_filter_pointer) == host_filter_pointer
    current_log, current_user = c.c_void_p(), c.c_void_p()
    sdl.SDL_GetLogOutputFunction(c.byref(current_log), c.byref(current_user))
    assert (current_log.value, current_user.value) == (original_log.value, original_log_user.value)
    context = c.c_void_p()
# Hold an initialization callback to prove that start/stop do not wait on worker callbacks.
block_ready = True
assert library.prosperity_create(c.byref(config), c.byref(context)) == 0
assert ready_entered.wait(5)
assert library.prosperity_boot(context, data + b'/cancel-before-load') == 0
assert library.prosperity_wait(context, 0) == -5
started = time.monotonic()
assert library.prosperity_request_stop(context) == 0
assert time.monotonic() - started < 0.5
release_ready.set()
assert library.prosperity_wait(context, 5000) == 0
replacement_filter = filter_type(lambda unused: 0)
replacement_pointer = c.cast(replacement_filter, c.c_void_p).value
kernel.SetUnhandledExceptionFilter(replacement_pointer)
assert library.prosperity_destroy(context) == 0
assert kernel.SetUnhandledExceptionFilter(original_filter) == replacement_pointer
assert logs and states and reentry and all(result == -2 for result in reentry)
content = (data_path / 'prosperity.log').read_text('utf-8')
assert '[embedding]' in content and 'state=6' in content
print('Embedding ABI 2 passed: async start/stop, timeout, cancellation, callbacks, reentry guards, repeated contexts, errors, LocalFolder log and host handler restoration')
print(json.dumps({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(runtime.glob('*.dll'))}, indent=2))
