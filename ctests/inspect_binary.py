"""Inspect PE imports to verify the portable release needs only system DLLs."""
import json
import struct
from pathlib import Path


def imports(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Not a PE binary")
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if machine != 0x8664 or struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Expected Windows x64 PE32+")
    table = optional + optional_size

    def offset(rva):
        for index in range(sections):
            virtual_size, address, raw_size, raw_offset = struct.unpack_from("<IIII", data, table + index * 40 + 8)
            if address <= rva < address + max(virtual_size, raw_size):
                return raw_offset + rva - address
        raise ValueError(f"Unmapped RVA: {rva}")

    current = offset(struct.unpack_from("<I", data, optional + 120)[0])
    names = []
    while True:
        descriptor = struct.unpack_from("<IIIII", data, current)
        if not any(descriptor):
            break
        name = offset(descriptor[3])
        names.append(data[name:data.index(b"\0", name)].decode("ascii"))
        current += 20
    subsystem = struct.unpack_from("<H", data, optional + 68)[0]
    expected = 2 if path.name == "netcollect.exe" else 3
    if subsystem != expected:
        raise ValueError(f"Incorrect PE subsystem {subsystem} for {path.name}")
    return {"file": path.name, "bytes": len(data), "machine": "x64", "subsystem": "GUI" if subsystem == 2 else "console", "imports": names}


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    result = [imports(root / "build" / name) for name in ("netcollect.exe", "netcollect-cli.exe")]
    text = json.dumps(result, indent=2)
    (root / "test-output" / "binary-imports.json").write_text(text, encoding="utf-8")
    print(text)
