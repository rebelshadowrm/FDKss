"""Wrong-build refusal through the real injector, before any remote DLL load."""
import ctypes as C
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from windows_process import kernel, Module


def modules(pid):
    snapshot = kernel.CreateToolhelp32Snapshot(0x18, pid)
    if snapshot == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    try:
        entry = Module()
        entry.size = C.sizeof(entry)
        found = []
        valid = kernel.Module32FirstW(snapshot, C.byref(entry))
        while valid:
            found.append(entry.name.lower())
            valid = kernel.Module32NextW(snapshot, C.byref(entry))
        return found
    finally:
        kernel.CloseHandle(snapshot)


host, injector, dll = map(Path, sys.argv[1:])
with tempfile.TemporaryDirectory(prefix='fdkss-injector-refusal-') as temporary:
    for name in ('sora_1st.exe', 'sora_2nd.exe', 'ysx_pn.exe', 'ysx.exe', 'unsupported.exe'):
        target = Path(temporary) / name
        shutil.copy2(host, target)
        child = subprocess.Popen([str(target), '--hold'], creationflags=0x08000000)
        try:
            result = subprocess.run([str(injector), str(child.pid), str(dll)],
                                    capture_output=True, text=True, timeout=10)
            expected = ('no supported FDKss adapter' if name == 'unsupported.exe'
                        else 'does not match its pinned adapter build')
            assert result.returncode == 1 and expected in result.stderr, result
            assert child.poll() is None
            assert 'fdkss.dll' not in modules(child.pid)
        finally:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
print('Wrong builds and unsupported filenames refused before DLL loading')
