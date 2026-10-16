"""Compare sequential and concurrent collection against local SSH devices."""
import ctypes
import json
import subprocess
import time
from ctypes import wintypes

from integration import ARTIFACTS, EXE, DeviceServer, workbook
from telnet_fixture import TelnetDeviceServer


class Memory(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [(name, ctypes.c_size_t) for name in ("peak", "working", "qpeak", "q", "ppeak", "p", "page", "pagepeak")]


def main(mixed=False):
    directory = ARTIFACTS / ("benchmark-mixed" if mixed else "benchmark")
    directory.mkdir(parents=True, exist_ok=True)
    servers = [DeviceServer("Bench-SSH"), TelnetDeviceServer("Bench-Telnet-GBK", encoding="gbk"), TelnetDeviceServer("Bench-Telnet-Password", mode="password", username="")] if mixed else [DeviceServer(f"Bench-{i}") for i in range(3)]
    metrics = []
    try:
        template = directory / "benchmark.xlsx"
        workbook(template, servers, ["display version", "display big", "display slow"])
        get_memory = ctypes.windll.psapi.GetProcessMemoryInfo
        get_memory.argtypes = [wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD]
        get_memory.restype = wintypes.BOOL
        for workers in (1, 3):
            start = time.perf_counter()
            with (directory / f"workers-{workers}.log").open("w", encoding="utf-8") as log:
                process = subprocess.Popen([str(EXE), "--input", str(template), "--output", str(directory / f"workers-{workers}"), "--workers", str(workers)], stdout=log, stderr=log)
                peak = 0
                while process.poll() is None:
                    memory = Memory()
                    memory.cb = ctypes.sizeof(memory)
                    if get_memory(wintypes.HANDLE(int(process._handle)), ctypes.byref(memory), ctypes.sizeof(memory)):
                        peak = max(peak, memory.peak)
                    if time.perf_counter() - start > 35:
                        process.kill()
                        process.wait()
                        raise RuntimeError("Benchmark timed out")
                    time.sleep(0.02)
                if process.returncode:
                    raise RuntimeError(f"Collector failed: exit {process.returncode}")
            metrics.append({"workers": workers, "devices": 3, "commands": 9, "elapsed_seconds": round(time.perf_counter() - start, 3), "peak_working_set_mib": round(peak / (1024 * 1024), 2)})
        result = {"protocols": [getattr(server, "protocol", "SSH") for server in servers], "runs": metrics, "speedup": round(metrics[0]["elapsed_seconds"] / metrics[1]["elapsed_seconds"], 2)}
        (directory / "metrics.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps(result, indent=2))
    finally:
        for server in servers:
            server.close()


if __name__ == "__main__":
    import sys
    main(mixed="--mixed" in sys.argv)
