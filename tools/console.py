"""Run Lua in a loaded FDKss mod through the local, user-restricted live console."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import sys
import json

K = C.WinDLL('kernel32', use_last_error=True)


class Overlapped(C.Structure):
    _fields_ = [('internal', C.c_size_t), ('internal_high', C.c_size_t),
                ('offset', W.DWORD), ('offset_high', W.DWORD), ('event', W.HANDLE)]


def configure(name, result, *arguments):
    function = getattr(K, name)
    function.restype, function.argtypes = result, arguments
    return function


wait_pipe = configure('WaitNamedPipeW', W.BOOL, W.LPCWSTR, W.DWORD)
open_pipe = configure('CreateFileW', W.HANDLE, W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE)
close = configure('CloseHandle', W.BOOL, W.HANDLE)
create_event = configure('CreateEventW', W.HANDLE, C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR)
wait_event = configure('WaitForSingleObject', W.DWORD, W.HANDLE, W.DWORD)
get_result = configure('GetOverlappedResult', W.BOOL, W.HANDLE, C.POINTER(Overlapped), C.POINTER(W.DWORD), W.BOOL)
cancel = configure('CancelIoEx', W.BOOL, W.HANDLE, C.POINTER(Overlapped))
read = configure('ReadFile', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(Overlapped))
write = configure('WriteFile', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(Overlapped))
set_mode = configure('SetNamedPipeHandleState', W.BOOL, W.HANDLE, C.POINTER(W.DWORD), C.c_void_p, C.c_void_p)


def transfer(pipe, function, buffer, length, timeout):
    event = create_event(None, True, False, None)
    if not event:
        raise C.WinError(C.get_last_error())
    operation = Overlapped(event=event)
    transferred = W.DWORD()
    try:
        immediate = function(pipe, buffer, length, C.byref(transferred), C.byref(operation))
        if not immediate:
            error = C.get_last_error()
            if error != 997:
                raise C.WinError(error)
            if wait_event(event, timeout) != 0:
                cancel(pipe, C.byref(operation))
                get_result(pipe, C.byref(operation), C.byref(transferred), True)
                raise TimeoutError('FDKss console timed out; the update thread may be paused')
        if not get_result(pipe, C.byref(operation), C.byref(transferred), False):
            raise C.WinError(C.get_last_error())
        return transferred.value
    finally:
        close(event)


def execute(pid, mod, source, timeout=10000):
    payload = (mod + '\n' + source).encode('utf-8')
    if not 0 < len(payload) <= 65536 or b'\0' in payload:
        raise ValueError('request must be at most 64 KiB and contain no NUL bytes')
    name = rf'\\.\pipe\FDKss-{pid}'
    if not wait_pipe(name, timeout):
        raise C.WinError(C.get_last_error())
    pipe = open_pipe(name, 0xc0000000, 0, None, 3, 0x40000000, None)
    if pipe == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    try:
        mode = W.DWORD(2)
        if not set_mode(pipe, C.byref(mode), None, None):
            raise C.WinError(C.get_last_error())
        outgoing = C.create_string_buffer(payload)
        sent = transfer(pipe, write, outgoing, len(payload), timeout)
        if sent != len(payload):
            raise OSError('incomplete console request')
        incoming = C.create_string_buffer(65536)
        size = transfer(pipe, read, incoming, len(incoming), timeout)
        acknowledgment = C.create_string_buffer(b'\x01')
        transfer(pipe, write, acknowledgment, 1, timeout)
        status, separator, result = incoming.raw[:size].decode('utf-8', errors='replace').partition('\n')
        if not separator or status not in ('OK', 'ERROR'):
            raise OSError('invalid console response')
        if status == 'ERROR':
            raise RuntimeError(result)
        return result
    finally:
        close(pipe)


def main():
    if sys.argv[1:] == ['--stdio']:
        # Private transport for GUI clients. Keep the Win32 pipe client out of Tk's process.
        for line in sys.stdin:
            try:
                request = json.loads(line)
                result = execute(request['pid'], request['mod'], request['source'], request.get('timeout', 10000))
                reply = {'ok': True, 'result': result}
            except Exception as error:
                reply = {'ok': False, 'error': str(error)}
            print(json.dumps(reply), flush=True)
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('mod', help='Loaded mod whose Lua state receives the code')
    parser.add_argument('source', nargs='?', help='Lua source; omitted for an interactive prompt')
    parser.add_argument('--timeout', type=float, default=10, help='Timeout in seconds (0.1..60)')
    args = parser.parse_args()
    if not 0.1 <= args.timeout <= 60:
        parser.error('timeout must be between 0.1 and 60 seconds')
    timeout = int(args.timeout * 1000)
    if args.source is not None:
        try:
            print(execute(args.pid, args.mod, args.source, timeout))
        except (OSError, RuntimeError, ValueError, TimeoutError) as error:
            print(error, file=sys.stderr)
            return 1
    else:
        print('Lua console. Use return for values; quit or Ctrl+C to exit.')
        try:
            while True:
                source = input(f'{args.mod}> ')
                if source == 'quit':
                    break
                try:
                    print(execute(args.pid, args.mod, source, timeout))
                except (OSError, RuntimeError, ValueError, TimeoutError) as error:
                    print(error, file=sys.stderr)
        except (EOFError, KeyboardInterrupt):
            pass
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
