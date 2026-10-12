"""Read-only Windows process helpers scoped to the supported Sky executables."""
import ctypes as C
from ctypes import wintypes as W
from pathlib import Path
import struct

kernel = C.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
kernel.OpenProcess.restype = W.HANDLE
kernel.CloseHandle.argtypes = [W.HANDLE]
kernel.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
kernel.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
kernel.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
kernel.CreateToolhelp32Snapshot.restype = W.HANDLE


class Module(C.Structure):
    _fields_ = [("size", W.DWORD), ("module_id", W.DWORD), ("pid", W.DWORD),
                ("global_usage", W.DWORD), ("process_usage", W.DWORD),
                ("base", C.c_void_p), ("image_size", W.DWORD), ("module", W.HMODULE),
                ("name", W.WCHAR * 256), ("path", W.WCHAR * 260)]


class Region(C.Structure):
    _fields_ = [("base", C.c_void_p), ("allocation_base", C.c_void_p),
                ("allocation_protect", W.DWORD), ("size", C.c_size_t),
                ("state", W.DWORD), ("protect", W.DWORD), ("kind", W.DWORD)]


kernel.Module32FirstW.argtypes = [W.HANDLE, C.POINTER(Module)]
kernel.Module32NextW.argtypes = [W.HANDLE, C.POINTER(Module)]
kernel.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(Region), C.c_size_t]
kernel.VirtualQueryEx.restype = C.c_size_t


class Process:
    def __init__(self, pid):
        self.pid = pid
        self.handle = kernel.OpenProcess(0x410, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        path = C.create_unicode_buffer(32768)
        length = W.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(self.handle, 0, path, C.byref(length)):
            self.close()
            raise C.WinError(C.get_last_error())
        self.path = Path(path.value)
        if self.path.name.lower() not in ("sora_1st.exe", "sora_2nd.exe"):
            self.close()
            raise ValueError("Only sora_1st.exe and sora_2nd.exe are supported")
        self.modules = self._modules()
        self.base = next(item["base"] for item in self.modules if item["name"].lower() == self.path.name.lower())

    def close(self):
        if self.handle:
            kernel.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _modules(self):
        snapshot = kernel.CreateToolhelp32Snapshot(0x18, self.pid)
        if snapshot == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        result = []
        try:
            item = Module()
            item.size = C.sizeof(item)
            valid = kernel.Module32FirstW(snapshot, C.byref(item))
            while valid:
                result.append({"name": item.name, "path": item.path,
                               "base": item.base, "size": item.image_size})
                valid = kernel.Module32NextW(snapshot, C.byref(item))
        finally:
            kernel.CloseHandle(snapshot)
        return result

    def read(self, address, size):
        if size < 0 or size > 16 * 1024 * 1024:
            raise ValueError("read size must be between zero and 16 MiB")
        buffer = C.create_string_buffer(size)
        count = C.c_size_t()
        if not kernel.ReadProcessMemory(self.handle, address, buffer, size, C.byref(count)) or count.value != size:
            raise OSError(f"Could not read {size} bytes at 0x{address:x}: Windows error {C.get_last_error()}")
        return buffer.raw[:size]

    def pointer(self, address):
        return struct.unpack("<Q", self.read(address, 8))[0]

    def class_name(self, address):
        try:
            table = self.pointer(address)
            locator = self.pointer(table - 8)
            signature, offset, constructor, type_rva, hierarchy_rva, self_rva = struct.unpack("<6I", self.read(locator, 24))
            if signature != 1 or locator - self_rva != self.base:
                return None
            return self.read(self.base + type_rva + 16, 256).split(b"\0", 1)[0].decode("ascii", errors="replace")
        except (OSError, ValueError):
            return None

    def regions(self, private_only=True):
        address = 0
        while address < 0x7FFFFFFF0000:
            region = Region()
            if not kernel.VirtualQueryEx(self.handle, address, C.byref(region), C.sizeof(region)):
                break
            base = region.base or 0
            if region.size == 0:
                break
            # Committed readable pages; exclude PAGE_NOACCESS and PAGE_GUARD.
            if region.state == 0x1000 and not (region.protect & 0x101) and (
                not private_only or region.kind == 0x20000
            ):
                yield base, region.size
            address = base + region.size
