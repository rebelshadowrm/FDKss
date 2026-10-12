#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "commands.h"
#include "scripts.h"
#include "../command_catalog.h"
#include <cstdio>
#include <string>
#include <algorithm>
namespace {
using command_catalog::Profile;
#define FDKSS_COMMAND_GROUP(group,table,scan,capacity,mask,offset,type) {group,table,scan,capacity,mask,offset,type},
constexpr Profile first[]{
#include "fdkss/sky1_command_groups.inc"
}, second[]{
#include "fdkss/sky2_command_groups.inc"
};
#undef FDKSS_COMMAND_GROUP
bool read(std::uintptr_t address, void *value, std::size_t size) {
    SIZE_T copied=0; return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),value,size,&copied) && copied==size;
}
std::uintptr_t pointer(std::uintptr_t address) { std::uintptr_t value=0; read(address,&value,8); return value; }
int fail(const char *text,char *error,std::size_t size) { std::snprintf(error,size,"%s",text); return 1; }
struct Context { std::uintptr_t base=0, manager=0; std::size_t size=0; bool sky2=false; };
bool context(void *data,Context &out) {
    out.sky2=data!=nullptr;
    if (data && static_cast<const sky1_scripts::CatalogProfile *>(data)->manager_global!=0xc60e58) return false;
    out.base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto framework=pointer(out.base+(out.sky2 ? 0xc60ec0 : 0x8ff030));
    DWORD worker=0;
    if (!framework || pointer(framework)!=out.base+(out.sky2 ? 0xb1ad08 : 0x802d80) ||
        !read(framework+(out.sky2 ? 0xa0 : 0x68),&worker,4) || worker!=GetCurrentThreadId()) return false;
    out.manager=pointer(out.base+(out.sky2 ? 0xc60e58 : 0x8fefc8));
    if (!out.manager || pointer(out.manager)!=out.base+(out.sky2 ? 0xb12fd0 : 0x7fc400)) return false;
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!read(out.base,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
        !read(out.base+dos.e_lfanew,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE) return false;
    out.size=nt.OptionalHeader.SizeOfImage; return out.size!=0;
}
bool entries(const Context &ctx,unsigned group,std::vector<command_catalog::Entry> &values) {
    const auto object=pointer(ctx.manager+0x20+group*8);
    const auto &profile=(ctx.sky2 ? second : first)[group];
    if (!command_catalog::snapshot(ctx.base,object,ctx.size,profile,read,values)) return false;
    for (const auto &entry : values) if (entry.words[0]) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void *>(entry.words[0]),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    }
    return pointer(ctx.manager+0x20+group*8)==object;
}
int emit_text(const std::string &text,fdkss_result_fn emit,void *sink) {
    fdkss_value value{}; value.type=FDKSS_STRING; value.string=text.data(); value.string_size=text.size(); return emit(sink,&value);
}
std::string entry_json(const Context &ctx,unsigned group,unsigned command,const command_catalog::Entry &entry) {
    char raw[49]{};
    const auto *bytes=reinterpret_cast<const unsigned char *>(entry.words.data());
    for (unsigned i=0;i<24;++i) std::snprintf(raw+i*2,3,"%02x",bytes[i]);
    return "{\"group\":"+std::to_string(group)+",\"command\":"+std::to_string(command)+",\"handler_rva\":"+std::to_string(entry.words[0]-ctx.base)+",\"raw_mask\":"+std::to_string(entry.mask)+",\"raw_entry_hex\":\""+raw+"\"}";
}
}
namespace native_commands {
int FDKSS_CALL groups(void *data,const fdkss_value *,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    Context ctx;
    if (count || !context(data,ctx)) return fail("command catalog requires no arguments on the verified owner thread",error,size);
    std::string result="[";
    for (unsigned group=0;group<28;++group) {
        std::vector<command_catalog::Entry> values;
        if (!entries(ctx,group,values)) return fail("native command group profile changed or unreadable",error,size);
        const auto &profile=(ctx.sky2 ? second : first)[group];
        const auto implemented=std::count_if(values.begin(),values.end(),[](const auto &entry){return entry.words[0]!=0;});
        if (group) result+=',';
        result+="{\"group\":"+std::to_string(group)+",\"class\":\""+profile.type+"\",\"slot_count\":"+std::to_string(profile.capacity)+",\"implemented_count\":"+std::to_string(implemented)+"}";
    }
    result+=']'; return emit_text(result,emit,sink);
}
int FDKSS_CALL info(void *data,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    Context ctx;
    if (count!=2 || args[0].type!=FDKSS_INTEGER || args[0].integer<0 || args[0].integer>=28 ||
        args[1].type!=FDKSS_INTEGER || args[1].integer<0 || args[1].integer>=256 || !context(data,ctx))
        return fail("command info expects group 0..27 and slot 0..255 on the verified owner thread",error,size);
    std::vector<command_catalog::Entry> values;
    const auto group=static_cast<unsigned>(args[0].integer), command=static_cast<unsigned>(args[1].integer);
    if (!entries(ctx,group,values)) return fail("native command group profile changed or unreadable",error,size);
    if (command>=values.size() || !values[command].words[0]) { fdkss_value nil{}; return emit(sink,&nil); }
    return emit_text(entry_json(ctx,group,command,values[command]),emit,sink);
}
int FDKSS_CALL page(void *data,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    Context ctx;
    if (count!=3 || args[0].type!=FDKSS_INTEGER || args[0].integer<0 || args[0].integer>=28 ||
        args[1].type!=FDKSS_INTEGER || args[1].integer<0 || args[1].integer>256 ||
        args[2].type!=FDKSS_INTEGER || args[2].integer<1 || args[2].integer>32 || !context(data,ctx))
        return fail("command page expects group 0..27, cursor 0..256 and limit 1..32 on the verified owner thread",error,size);
    std::vector<command_catalog::Entry> values;
    const auto group=static_cast<unsigned>(args[0].integer), begin=static_cast<unsigned>(args[1].integer);
    if (!entries(ctx,group,values)) return fail("native command group profile changed or unreadable",error,size);
    const auto end=std::min<std::size_t>(values.size(),begin+static_cast<unsigned>(args[2].integer));
    std::string rows="["; bool first_row=true;
    for (std::size_t i=begin;i<end;++i) if (values[i].words[0]) {
        if (!first_row) rows+=',';
        first_row=false;
        rows+=entry_json(ctx,group,static_cast<unsigned>(i),values[i]);
    }
    rows+=']';
    const auto next=end<values.size() ? static_cast<int>(end) : -1;
    return emit_text("{\"next\":"+std::to_string(next)+",\"entries\":"+rows+"}",emit,sink);
}
}
