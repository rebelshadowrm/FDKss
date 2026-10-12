"""Read-only PE inventory for FDK adapter work; never modifies or launches a game."""
import argparse
import hashlib
import json
from pathlib import Path
import re

import pefile


def inspect(path: Path) -> dict:
    data = path.read_bytes()
    image = pefile.PE(data=data)
    imports = {}
    for module in getattr(image, "DIRECTORY_ENTRY_IMPORT", []):
        imports[module.dll.decode("ascii", errors="replace")] = [
            {
                "name": symbol.name.decode("ascii", errors="replace") if symbol.name else None,
                "ordinal": symbol.ordinal,
                "iat_rva": hex(symbol.address - image.OPTIONAL_HEADER.ImageBase),
            }
            for symbol in module.imports
        ]
    strings = re.findall(rb"[ -~]{8,}", data)
    fdk_paths = sorted({
        text.decode("ascii") for text in strings
        if b"\\src\\fdk\\" in text.lower()
    })
    rtti = sorted({
        text.decode("ascii", errors="replace")
        for text in re.findall(rb"\.\?AV[^\x00]{3,250}", data)
        if b"@fdk@@" in text
    })
    return {
        "file": path.name,
        "sha256": hashlib.sha256(data).hexdigest(),
        "size_bytes": len(data),
        "machine": hex(image.FILE_HEADER.Machine),
        "coff_timestamp": image.FILE_HEADER.TimeDateStamp,
        "image_base": hex(image.OPTIONAL_HEADER.ImageBase),
        "entry_rva": hex(image.OPTIONAL_HEADER.AddressOfEntryPoint),
        "sections": [
            {"name": section.Name.rstrip(b"\0").decode("ascii", errors="replace"),
             "rva": hex(section.VirtualAddress), "virtual_size": section.Misc_VirtualSize}
            for section in image.sections
        ],
        "imports": imports,
        "fdk_source_paths": fdk_paths,
        "fdk_rtti_names": rtti,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executables", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, help="Write report JSON instead of stdout")
    args = parser.parse_args()
    result = json.dumps([inspect(path) for path in args.executables], indent=2)
    if args.output:
        args.output.write_text(result + "\n", encoding="utf-8")
    else:
        print(result)


if __name__ == "__main__":
    main()
