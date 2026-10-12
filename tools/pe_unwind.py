"""Group x64 version-1/2 runtime-function records by their chained unwind primary.

Metadata boundaries only: no instruction decoding, callable or ownership proof.
Noncontiguous parts remain separate; gaps are never included in a code body.
Format: https://learn.microsoft.com/en-us/cpp/build/exception-handling-x64
V1/V2 trailer layout: llvm/include/llvm/Support/Win64EH.h, UnwindInfo.
Unwind operations and stack reconstruction are deliberately not interpreted.
"""
import bisect
import struct


class FunctionFamilies:
    def __init__(self, image, records, executable):
        self.records = sorted(tuple(row) for row in records)
        self.starts = [row[0] for row in self.records]
        self.primary = {}
        self.families = {}
        known = set(self.records)
        if len(known) != len(self.records):
            raise ValueError('duplicate runtime-function record')
        if any(not 0 <= a < b <= len(image) for a, b in executable):
            raise ValueError('invalid executable section bounds')
        previous_end = 0
        parents = {}
        for row in self.records:
            begin, end, unwind = row
            if not 0 <= begin < end <= len(image) or begin < previous_end:
                raise ValueError('invalid or overlapping runtime-function bounds')
            previous_end = end
            if not any(a <= begin and end <= b for a, b in executable):
                raise ValueError('runtime-function record leaves executable sections')
            if unwind % 4 or not 0 < unwind <= len(image)-4:
                raise ValueError('unwind header is unaligned or outside image')
            version_flags, _, slots, _ = image[unwind:unwind+4]
            version, flags = version_flags & 7, version_flags >> 3
            if version not in (1, 2) or flags & ~7 or flags & 4 and flags & 3:
                raise ValueError('unsupported unwind version or incompatible flags')
            tail = unwind + 4 + ((slots+1) & ~1)*2
            size = 12 if flags & 4 else 4 if flags & 3 else 0
            if tail+size > len(image):
                raise ValueError('truncated unwind codes or trailer')
            if flags & 4:
                parent = struct.unpack_from('<III', image, tail)
                if parent not in known:
                    raise ValueError('chained parent is not an exact runtime-function record')
                parents[row] = parent
        for row in self.records:
            path, visited, current = [], set(), row
            while current not in self.primary:
                if current in visited or len(path) >= 64:
                    raise ValueError('cyclic or excessively deep unwind chain')
                visited.add(current)
                path.append(current)
                parent = parents.get(current)
                if parent is None:
                    primary = current
                    break
                current = parent
            else:
                primary = self.primary[current]
            for item in path:
                self.primary[item] = primary
            self.families.setdefault(primary, []).append(row)

    def containing(self, address):
        index = bisect.bisect_right(self.starts, address)-1
        if index < 0 or address >= self.records[index][1]:
            raise ValueError('address is outside registered runtime-function ranges')
        row = self.records[index]
        primary = self.primary[row]
        return primary, tuple(self.families[primary])

    def contiguous(self, address):
        """Return one span only when the metadata family has no gaps."""
        primary, records = self.containing(address)
        if records[0] != primary:
            raise ValueError('primary is not the first range; preserve separate ranges')
        if any(left[1] != right[0] for left, right in zip(records, records[1:])):
            raise ValueError('noncontiguous function family; preserve separate ranges')
        return primary[0], records[-1][1]


def from_pe(pe):
    if pe.FILE_HEADER.Machine != 0x8664 or pe.OPTIONAL_HEADER.Magic != 0x20b:
        raise ValueError('only x64 PE32+ unwind metadata is supported')
    image = pe.get_memory_mapped_image()
    records = [(e.struct.BeginAddress, e.struct.EndAddress, e.struct.UnwindData)
               for e in pe.DIRECTORY_ENTRY_EXCEPTION]
    executable = [(s.VirtualAddress, s.VirtualAddress+s.Misc_VirtualSize)
                  for s in pe.sections if s.Characteristics & 0x20000000]
    return FunctionFamilies(image, records, executable)
