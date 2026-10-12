#pragma once
#include "../class_metadata.h"
#include "fdkss/api.h"
#include <windows.h>
#include <cstdio>

namespace live_class_metadata {
inline int inspect(std::uintptr_t image, std::uintptr_t object, const fdkss_value *args,
    std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    auto fail=[&] {std::snprintf(error,capacity,"class metadata requires a current root/actor and valid RTTI index");return 1;};
    if (!object || count!=2 || args[1].type!=FDKSS_INTEGER || args[1].integer<0 || args[1].integer>=1024) return fail();
    auto read=[](std::uintptr_t address,void *dest,std::size_t length) {
        SIZE_T copied=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),dest,length,&copied) && copied==length;
    };
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 pe{};std::uintptr_t table=0,after=0;
    if (!read(image,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
        !read(image+dos.e_lfanew,&pe,sizeof(pe)) || pe.Signature!=IMAGE_NT_SIGNATURE ||
        pe.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || pe.OptionalHeader.SizeOfImage>512*1024*1024 ||
        !read(object,&table,8)) return fail();
    class_metadata::Row row;
    if (!class_metadata::inspect(image,pe.OptionalHeader.SizeOfImage,table,static_cast<std::uint32_t>(args[1].integer),read,row) ||
        !read(object,&after,8) || table!=after) return fail();
    fdkss_value value{};value.type=FDKSS_STRING;value.string=row.name.data();value.string_size=row.name.size();
    if (emit(sink,&value)) return 1;
    value.string=row.base_name.data();value.string_size=row.base_name.size();if (emit(sink,&value)) return 1;
    value.type=FDKSS_INTEGER;
    for (const std::int64_t integer : {static_cast<std::int64_t>(row.count),static_cast<std::int64_t>(row.flags),
        static_cast<std::int64_t>(row.object_offset),static_cast<std::int64_t>(row.constructor_offset),
        static_cast<std::int64_t>(row.contained),static_cast<std::int64_t>(row.member_offset),
        static_cast<std::int64_t>(row.vbtable_offset),static_cast<std::int64_t>(row.virtual_displacement),static_cast<std::int64_t>(row.attributes)}) {
        value.integer=integer;if (emit(sink,&value)) return 1;
    }
    return 0;
}
}
