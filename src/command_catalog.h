#pragma once
#include <cstddef>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
namespace command_catalog {
struct Profile { unsigned group; std::uintptr_t table, scan; unsigned capacity; std::uintptr_t mask; unsigned mask_offset; const char *type; };
struct Entry { std::array<std::uint64_t, 3> words{}; std::uint32_t mask = 0; };
inline std::vector<unsigned char> scan_code(unsigned capacity) {
    if (!capacity || capacity > 256) return {};
    std::vector<unsigned char> code{0x33,0xc0,0x48,0x83,0xc1,8,0x48,0x83,0x39,0,0x74,static_cast<unsigned char>(capacity <= 127 ? 16 : 18),0xff,0xc0,0x48,0x83,0xc1,0x18};
    if (capacity <= 127) code.insert(code.end(), {0x83,0xf8,static_cast<unsigned char>(capacity)});
    else { code.push_back(0x3d); for (unsigned i=0;i<4;++i) code.push_back(static_cast<unsigned char>(capacity >> (i*8))); }
    code.insert(code.end(), {0x72,static_cast<unsigned char>(capacity <= 127 ? 0xef : 0xed),0xb8,0xff,0xff,0xff,0xff,0xc3}); return code;
}
inline std::vector<unsigned char> mask_code(unsigned offset) {
    std::vector<unsigned char> code{0x8b,0xc2,0x8b};
    if (offset <= 127) code.insert(code.end(), {0x44,0x81,static_cast<unsigned char>(offset)});
    else { code.insert(code.end(), {0x84,0x81}); for (unsigned i=0;i<4;++i) code.push_back(static_cast<unsigned char>(offset >> (i*8))); }
    code.push_back(0xc3); return code;
}
// Read callback copies bytes; no engine methods or handlers are called.
template<class Read> bool snapshot(std::uintptr_t base, std::uintptr_t object, std::size_t image_size,
    const Profile &profile, Read read, std::vector<Entry> &output) {
    output.clear();
    if (!base || !object || !profile.capacity || profile.capacity > 256 || profile.mask_offset != 8+profile.capacity*24) return false;
    std::uintptr_t table=0, scan=0, mask=0;
    if (!read(object,&table,8) || table != base+profile.table || !read(table+16,&scan,8) || scan != base+profile.scan ||
        !read(table+24,&mask,8) || mask != base+profile.mask) return false;
    const auto expected_scan=scan_code(profile.capacity), expected_mask=mask_code(profile.mask_offset);
    std::array<unsigned char,32> actual{};
    if (!read(scan,actual.data(),expected_scan.size()) || std::memcmp(actual.data(),expected_scan.data(),expected_scan.size()) ||
        !read(mask,actual.data(),expected_mask.size()) || std::memcmp(actual.data(),expected_mask.data(),expected_mask.size())) return false;
    std::vector<Entry> copy(profile.capacity);
    for (unsigned i=0;i<profile.capacity;++i) {
        if (!read(object+8+i*24,copy[i].words.data(),24) || !read(object+profile.mask_offset+i*4,&copy[i].mask,4)) return false;
        const auto handler=copy[i].words[0];
        if (handler && (handler < base || handler-base >= image_size)) return false;
    }
    // Reject object/table replacement during the copy.
    std::uintptr_t current=0;
    if (!read(object,&current,8) || current != table) return false;
    output=std::move(copy); return true;
}
}
