"""Verify the packaged GUI executable starts and closes with its real entry point."""
import ctypes
import subprocess
from ctypes import wintypes
from pathlib import Path

root = Path(__file__).resolve().parents[1]
directory = root / "dist" / "NetCollect-C"
version = (root / "VERSION").read_text(encoding="utf-8").strip()
check_version = subprocess.run([str(directory / "netcollect-cli.exe"), "--version"], capture_output=True, encoding="utf-8", timeout=10)
if check_version.returncode or check_version.stdout.strip() != f"NetCollcet-C {version}":
    raise RuntimeError("Packaged executable version does not match VERSION")
cli = subprocess.run([str(directory / "netcollect-cli.exe"), "--check", str(directory / "C版采集模板.xlsx")], capture_output=True, encoding="utf-8", timeout=10)
if cli.returncode:
    raise RuntimeError(cli.stdout + cli.stderr)
print(cli.stdout.strip())
logs = directory / "logs"
before = set(logs.glob("application_*.log"))

user = ctypes.windll.user32
user.WaitForInputIdle.argtypes = [wintypes.HANDLE, wintypes.DWORD]
user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
user.GetDlgItem.restype = wintypes.HWND
user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
process = subprocess.Popen([str(directory / "netcollect.exe")], startupinfo=startup)
windows = []
try:
    if user.WaitForInputIdle(wintypes.HANDLE(int(process._handle)), 10000):
        raise RuntimeError("GUI did not become idle")

    @callback_type
    def visit(window, unused):
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(pid))
        if pid.value == process.pid and user.GetDlgItem(window, 101):
            windows.append(window)
        return True

    user.EnumWindows(visit, 0)
    if len(windows) != 1:
        raise RuntimeError("Expected one native collector window")
    for control in (101, 102, 105, 106, 107, 108, 109, 110, 111):
        if not user.GetDlgItem(windows[0], control):
            raise RuntimeError(f"Missing GUI control {control}")
    user.PostMessageW(windows[0], 0x10, 0, 0)
    if process.wait(timeout=10):
        raise RuntimeError("GUI exit failure")
    created = set(logs.glob("application_*.log")) - before
    if len(created) != 1:
        raise RuntimeError("Expected a new application log for GUI startup")
    text = created.pop().read_text(encoding="utf-8-sig")
    if not all(event in text for event in ("APPLICATION_START", "GUI_OPEN", "GUI_CLOSED", "APPLICATION_EXIT code=0")):
        raise RuntimeError("GUI lifecycle events missing from application log")
    print("Packaged GUI startup, native controls, application log and normal close: PASS")
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
