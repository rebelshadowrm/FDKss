"""Capture a Sky game window or send a bounded key press to that window."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import time
from windows_process import Process

user = C.WinDLL("user32", use_last_error=True)
user.SetProcessDPIAware()
user.GetForegroundWindow.restype = W.HWND
user.SetForegroundWindow.argtypes = [W.HWND]
user.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
user.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
user.IsWindowVisible.argtypes = [W.HWND]
EnumCallback = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
user.EnumWindows.argtypes = [EnumCallback, W.LPARAM]


class Keyboard(C.Structure):
    _fields_ = [("vk", W.WORD), ("scan", W.WORD), ("flags", W.DWORD),
                ("time", W.DWORD), ("extra", C.c_size_t)]


class Mouse(C.Structure):
    _fields_ = [("dx", W.LONG), ("dy", W.LONG), ("data", W.DWORD),
                ("flags", W.DWORD), ("time", W.DWORD), ("extra", C.c_size_t)]


class Payload(C.Union):
    _fields_ = [("keyboard", Keyboard), ("mouse", Mouse)]


class Input(C.Structure):
    _fields_ = [("type", W.DWORD), ("payload", Payload)]


user.SendInput.argtypes = [W.UINT, C.POINTER(Input), C.c_int]


def window(pid):
    windows = []
    @EnumCallback
    def visit(hwnd, _):
        target = W.DWORD()
        user.GetWindowThreadProcessId(hwnd, C.byref(target))
        if target.value == pid and user.IsWindowVisible(hwnd):
            rect = W.RECT()
            user.GetWindowRect(hwnd, C.byref(rect))
            if rect.right - rect.left > 500 and rect.bottom - rect.top > 300:
                windows.append(hwnd)
        return True
    user.EnumWindows(visit, 0)
    if not windows:
        raise RuntimeError("No visible game window found")
    return windows[0]


def send_key(hwnd, key, hold):
    if user.GetForegroundWindow() != hwnd:
        raise RuntimeError("Game must have focus before sending a key")
    down = Input(type=1, payload=Payload(keyboard=Keyboard(vk=key)))
    up = Input(type=1, payload=Payload(keyboard=Keyboard(vk=key, flags=2)))
    if user.SendInput(1, C.byref(down), C.sizeof(Input)) != 1:
        raise C.WinError(C.get_last_error())
    try:
        time.sleep(hold)
    finally:
        user.SendInput(1, C.byref(up), C.sizeof(Input))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--screenshot")
    parser.add_argument("--key", help="Virtual-key number, decimal or hex")
    parser.add_argument("--hold", type=float, default=0.12)
    parser.add_argument("--wait", type=float, default=0.4)
    args = parser.parse_args()
    if not 0 <= args.hold <= 5 or not 0 <= args.wait <= 10:
        parser.error("hold must be 0..5 seconds and wait 0..10 seconds")
    with Process(args.pid):
        hwnd = window(args.pid)
        # Focus changes disturbed input in an early live session. This helper never
        # changes foreground ownership; the user must put the game in focus.
        if args.key:
            key = int(args.key, 0)
            if not 0 <= key <= 255:
                parser.error("virtual key must be 0..255")
            send_key(hwnd, key, args.hold)
        time.sleep(args.wait)
        if args.screenshot:
            if user.GetForegroundWindow() != hwnd:
                raise RuntimeError("Game lost focus; refusing to capture another foreground application")
            from PIL import ImageGrab
            rect = W.RECT()
            if not user.GetWindowRect(hwnd, C.byref(rect)):
                raise C.WinError(C.get_last_error())
            ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom)).save(args.screenshot)
            print(args.screenshot)


if __name__ == "__main__":
    main()
