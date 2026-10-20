"""Exercise the real DLL storage ABI without starting a game or UWP application."""
import argparse
import ctypes as c
from concurrent.futures import ThreadPoolExecutor
import json
import msvcrt
import os
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--vclibs', type=Path, required=True)
a = p.parse_args()
runtime = Path('Prosperity-UWP/Runtime/Release').resolve()
search = [os.add_dll_directory(str(x)) for x in (runtime, a.vclibs)]
dll = c.CDLL(str(runtime / 'prosperity.dll'))
P, U32, U64, I32 = c.c_void_p, c.c_uint32, c.c_uint64, c.c_int32
Ref = c.CFUNCTYPE(None, P)
Read = c.CFUNCTYPE(I32, P, U64, P, U64, c.POINTER(U64))
Size = c.CFUNCTYPE(I32, P, c.POINTER(U64))
Flush = c.CFUNCTYPE(I32, P)
class Stream(c.Structure):
    _fields_ = [('size', U32), ('user', P), ('retain', Ref), ('release', Ref),
                ('read', Read), ('write', Read), ('get_size', Size), ('flush', Flush)]
class Info(c.Structure):
    _fields_ = [('size', U32), ('flags', U32), ('file_size', U64), ('time', c.c_int64)]
class Opened(c.Structure):
    _fields_ = [('size', U32), ('handle', P), ('stream', Stream)]
Emit = c.CFUNCTYPE(I32, P, c.c_char_p, c.POINTER(Info))
Open = c.CFUNCTYPE(I32, P, c.c_char_p, U32, c.POINTER(Opened))
Stat = c.CFUNCTYPE(I32, P, c.c_char_p, c.POINTER(Info))
Begin = c.CFUNCTYPE(I32, P, c.c_char_p, c.POINTER(P))
Next = c.CFUNCTYPE(I32, P, Emit, P)
class Storage(c.Structure):
    _fields_ = [('size', U32), ('version', U32), ('user', P), ('retain', Ref), ('release', Ref),
                ('open', Open), ('stat', Stat), ('begin', Begin), ('next', Next), ('close', Ref)]
Frame = c.CFUNCTYPE(None, P, P, U32, U32, U32, U32)
Log = c.CFUNCTYPE(None, P, U32, c.c_char_p)
State = c.CFUNCTYPE(None, P, I32, I32, c.c_char_p)
class Config(c.Structure):
    _fields_ = [('size', U32), ('version', U32), ('directory', c.c_char_p), ('user', P),
                ('frame', Frame), ('log', Log), ('state', State)]
def bind(name, args, result=I32):
    f = getattr(dll, 'prosperity_' + name); f.argtypes = args; f.restype = result; return f
create = bind('create', [c.POINTER(Config), c.POINTER(P)])
wait = bind('wait', [P, U32]); destroy = bind('destroy', [P])
mount = bind('mount_storage', [P, c.c_char_p, c.POINTER(Storage)])
unmount = bind('unmount_storage', [P, c.c_char_p])
import_stream = bind('import_file_stream', [P, c.c_char_p, c.POINTER(Stream)])
import_handle = bind('import_file_handle', [P, c.c_char_p, P])
fopen = bind('file_open', [P, c.c_char_p, U32, c.POINTER(P)])
read = bind('file_read_at', [P, U64, P, U64, c.POINTER(U64)])
write = bind('file_write_at', [P, U64, P, U64, c.POINTER(U64)])
get_size = bind('file_get_size', [P, c.POINTER(U64)])
flush = bind('file_flush', [P]); close = bind('file_close', [P], None)
stat = bind('storage_stat', [P, c.c_char_p, c.POINTER(Info)])
listing = bind('storage_list', [P, c.c_char_p, Emit, P])
base = Path('build/uwp-storage-tests').resolve(); base.mkdir(parents=True, exist_ok=True)
callbacks = [Frame(lambda *x: None), Log(lambda *x: None), State(lambda *x: None)]
config = Config(c.sizeof(Config), 2, str(base / 'data').encode(), None, *callbacks)
ctx = P(); assert create(c.byref(config), c.byref(ctx)) == 0
assert wait(ctx, 5000) == 0
refs = [0]
@Ref
def retain(u): refs[0] += 1
@Ref
def release(u): refs[0] -= 1
virtual_size = 8 * 1024**3 + 123
@Read
def stream_read(u, offset, buffer, requested, count):
    n = min(requested, max(0, virtual_size - offset), 137)  # deliberate short reads
    data = bytes((offset + i) % 251 for i in range(n))
    c.memmove(buffer, data, n); count[0] = n
    return 0 if n else 1
@Size
def stream_size(u, size): size[0] = virtual_size; return 0
stream = Stream(c.sizeof(Stream), None, retain, release, stream_read, Read(), stream_size, Flush())
root = b'Z:/prosperity-broker-only'
assert import_stream(ctx, root + b'/single', c.byref(stream)) == 0
assert refs[0] == 1
def open_file(path, flags=1):
    f = P(); result = fopen(ctx, path, flags, c.byref(f)); assert result == 0, (path, result); return f
def verify(f, offset, length=4096):
    b = c.create_string_buffer(length); n = U64()
    assert read(f, offset, b, length, c.byref(n)) == 0
    assert n.value == length and b.raw == bytes((offset + i) % 251 for i in range(length))
f = open_file(root + b'/single')
verify(f, 5 * 1024**3 + 17)
n = U64(); b = c.create_string_buffer(32)
assert read(f, virtual_size, b, 32, c.byref(n)) == 1 and n.value == 0
assert write(f, 0, b, 32, c.byref(n)) == -2
assert read(f, 2**63, b, 1, c.byref(n)) == -5
assert unmount(ctx, root + b'/single') == 0 and refs[0] == 2
verify(f, 99); close(f); assert refs[0] == 0

cursors = {}; entries = [f'arquivo-{i:04d}-ç.bin'.encode() for i in range(513)]
@Open
def provider_open(u, path, flags, output):
    if path != b'sub/dados.bin': return -1
    if flags != 1: return -2
    retain(None); output[0].stream = stream; return 0
@Stat
def provider_stat(u, path, info):
    if path in (b'', b'sub'): info[0].flags = 33; info[0].file_size = 0; return 0
    if path == b'sub/dados.bin': info[0].flags = 1; info[0].file_size = virtual_size; return 0
    return -1
@Begin
def begin(u, path, out):
    if path not in (b'', b'sub'): return -1
    token = max(cursors, default=0) + 1; cursors[token] = 0; out[0] = token; return 0
@Next
def next_entry(cursor, emit, user):
    i = cursors[cursor]
    if i == len(entries): return 1
    cursors[cursor] += 1
    info = Info(c.sizeof(Info), 1, virtual_size, 0)
    return emit(user, entries[i], c.byref(info))
@Ref
def end(cursor): del cursors[cursor]
provider = Storage(c.sizeof(Storage), 1, None, retain, release, provider_open, provider_stat, begin, next_entry, end)
assert mount(ctx, root, c.byref(provider)) == 0
info = Info(); info.size = c.sizeof(info)
assert stat(ctx, root + b'/sub/dados.bin', c.byref(info)) == 0 and info.file_size == virtual_size
names = []
@Emit
def collect(u, name, info): names.append(name); return 0
assert listing(ctx, root + b'/sub', collect, None) == 0
assert names == entries and not cursors
@Emit
def cancel(*x): return -6
assert listing(ctx, root, cancel, None) == -6 and not cursors
handles = [open_file(root + b'/sub/./dados.bin') for _ in range(8)]
with ThreadPoolExecutor(max_workers=8) as pool:
    list(pool.map(lambda pair: verify(pair[1], 5 * 1024**3 + pair[0] * 8192), enumerate(handles)))
for h in handles: close(h)
assert fopen(ctx, root + b'/absent', 1, c.byref(P())) == -1
assert unmount(ctx, root) == 0 and refs[0] == 0

# Native import duplicates the handle; original may be closed immediately.
native = base / 'large-sparse.bin'
with native.open('wb') as out:
    k = c.WinDLL('kernel32', use_last_error=True)
    k.DeviceIoControl.argtypes = [P, U32, P, U32, P, U32, c.POINTER(U32), P]
    returned = U32()
    assert k.DeviceIoControl(msvcrt.get_osfhandle(out.fileno()), 0x900C4, None, 0, None, 0, c.byref(returned), None)
    out.seek(5 * 1024**3 + 7); out.write(b'broker-native-offset')
kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.CreateFileW.argtypes = [c.c_wchar_p, U32, U32, P, U32, U32, P]; kernel.CreateFileW.restype = P
kernel.CloseHandle.argtypes = [P]
h = kernel.CreateFileW(str(native), 0x80000000, 7, None, 3, 0x40000000, None)
assert h and h != P(-1).value
assert import_handle(ctx, root + b'/native', h) == 0; kernel.CloseHandle(h)
f = open_file(root + b'/native'); b = c.create_string_buffer(20); n = U64()
assert read(f, 5 * 1024**3 + 7, b, 20, c.byref(n)) == 0 and b.raw == b'broker-native-offset'
close(f); assert unmount(ctx, root + b'/native') == 0
native.unlink()

# CREATE alone preserves data, truncation is explicit; byte counts are exact.
path = str(base / 'roundtrip.bin').encode()
f = open_file(path, 1 | 2 | 4 | 8); b = c.create_string_buffer(b'abcdef'); n = U64()
assert write(f, 0, b, 6, c.byref(n)) == 0 and n.value == 6
assert flush(f) == 0; close(f)
f = open_file(path, 1 | 2 | 4); length = U64()
assert get_size(f, c.byref(length)) == 0 and length.value == 6
assert read(f, 0, b, 6, c.byref(n)) == 0 and b.raw[:6] == b'abcdef'; close(f)
# Context destruction releases mounts; existing file references remain valid.
assert import_stream(ctx, root + b'/last', c.byref(stream)) == 0
f = open_file(root + b'/last')
assert destroy(ctx) == 0 and refs[0] == 2
verify(f, 8192); close(f); assert refs[0] == 0
report = dict(result='PASS', directory_entries=513, concurrent_files=8, stream_size=virtual_size,
              offset_above_4GiB=True, partial_reads=True, native_handle_ownership=True,
              cancellation=True, lifetime_after_unmount_and_destroy=True, create_preserves_content=True)
(base / 'result.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report))
