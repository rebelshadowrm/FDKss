"""Capture a read-only snapshot of known Sky 1st Chapter 1.04.2 engine roots."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
from windows_process import Process

EXPECTED_SHA256 = "4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf"
ROOTS = {
    "application": (0x8FBA50, ".?AVApplication@sora@@"),
    "framework": (0x8FF030, ".?AVFramework@fdk@@"),
    "fieldmap": (0x8FEF78, ".?AVManager@fieldmap@sora@@"),
    "script_manager": (0x8FEFC8, ".?AVManager@script@sora@@"),
}


def snapshot(process):
    result = {"time": time.time(), "pid": process.pid, "image_base": hex(process.base)}
    roots = {}
    for name, (rva, expected) in ROOTS.items():
        address = process.pointer(process.base + rva)
        found = process.class_name(address)
        roots[name] = {"slot_rva": hex(rva), "address": hex(address), "class": found, "valid": found == expected}
    result["roots"] = roots
    if roots["framework"]["valid"]:
        address = int(roots["framework"]["address"], 16)
        data = process.read(address, 0x90)
        result["timing"] = {
            "fps": struct.unpack_from("<f", data, 0x30)[0],
            "delta_seconds": struct.unpack_from("<f", data, 0x38)[0],
            "time_scale": struct.unpack_from("<f", data, 0x3C)[0],
            "worker_thread": struct.unpack_from("<I", data, 0x68)[0],
        }
    if roots["fieldmap"]["valid"]:
        manager = int(roots["fieldmap"]["address"], 16)
        members = {}
        for name, offset in {"scene": 0x108, "player_controller": 0x640}.items():
            address = process.pointer(manager + offset)
            members[name] = {"address": hex(address), "class": process.class_name(address)}
        result["fieldmap_members"] = members
        scene = int(members["scene"]["address"], 16)
        if members["scene"]["class"] == ".?AVScene@fieldmap@sora@@":
            result["map_id"] = process.read(scene + 0x30, 32).split(b"\0", 1)[0].decode("ascii", errors="replace")
        controller = int(members["player_controller"]["address"], 16)
        if members["player_controller"]["class"] == ".?AVPlayer@fieldmap@sora@@":
            actor = process.pointer(controller + 0x40)
            if process.class_name(actor) == ".?AVActor@chara@sora@@":
                result["player"] = {"address": hex(actor), "class": process.class_name(actor),
                                    "position": struct.unpack("<3f", process.read(actor + 0xD8, 12))}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--samples", type=int, default=1)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.samples <= 60 or not 0 <= args.interval <= 10:
        parser.error("samples must be 1..60 and interval 0..10 seconds")
    with Process(args.pid) as process:
        if hashlib.sha256(process.path.read_bytes()).hexdigest() != EXPECTED_SHA256:
            parser.error("Unsupported executable build")
        results = []
        for index in range(args.samples):
            if index:
                time.sleep(args.interval)
            results.append(snapshot(process))
    text = json.dumps(results, indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)


if __name__ == "__main__":
    main()
