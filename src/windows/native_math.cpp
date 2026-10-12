#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "native_math.h"
#include "../vm_pure.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <xmmintrin.h>
namespace native_math {
namespace {
bool read(std::uintptr_t at,void *out,std::size_t length) {
    SIZE_T copied=0;return at && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(at),out,length,&copied) && copied==length;
}
bool executable(std::uintptr_t at,std::size_t length=1) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(reinterpret_cast<void *>(at),&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
        (region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) &&
        at>=reinterpret_cast<std::uintptr_t>(region.BaseAddress) &&
        length<=region.RegionSize-(at-reinterpret_cast<std::uintptr_t>(region.BaseAddress));
}
int fail(const char *message,char *error,std::size_t size){std::snprintf(error,size,"%s",message);return 1;}
bool profile(const Context &context,std::uintptr_t &command,unsigned index=39) {
    const auto &p=context.second ? second : first;
    const auto &selected=context.second ? select_second : select_first;
    if(index!=23 && index!=25 && index!=26 && index!=39)return false;
    const auto entry=index==23 ? selected.square_root : index==25 ? selected.minimum : index==26 ? selected.maximum : p.handler;
    const auto hex=index==23 ? (context.second ? sqrt_sky2_code : sqrt_sky1_code) :
        index==25 ? min_code : index==26 ? max_code : context.second ? second_code : first_code;
    const auto length=std::strlen(hex)/2;
    const auto &group=context.second ? private_timer::second_read : private_timer::first_read;
    const auto base=context.image;
    std::uintptr_t manager=0,manager_table=0,table=0,scan=0,mask=0,handler=0,current=0;
    std::uint64_t adjustment=0;std::uint32_t extra=0,metadata=0;
    if (!read(base+group.manager,&manager,8) || !manager || !read(manager,&manager_table,8) ||
        manager_table!=base+(context.second ? 0xb12fd0 : 0x7fc400) || !read(manager+0x20,&command,8) || !command ||
        !read(command,&table,8) || table!=base+group.table ||
        !read(table+16,&scan,8) || scan!=base+group.scan || !read(table+24,&mask,8) || mask!=base+group.mask ||
        !read(command+8+index*24,&handler,8) || handler!=base+entry ||
        !read(command+8+index*24+8,&adjustment,8) || adjustment || !read(command+8+index*24+16,&extra,4) || extra ||
        !read(command+group.mask_offset+index*4,&metadata,4) || metadata!=1 ||
        !executable(handler,length) || !executable(scan,32) || !executable(mask,32))return false;
    std::array<unsigned char,280> body{};
    if (length>body.size() || !read(handler,body.data(),length))return false;
    const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
    for(std::size_t i=0;i<length;++i)if(body[i]!=nibble(hex[2*i])*16+nibble(hex[2*i+1]))return false;
    std::array<std::uint32_t,4> signs{};
    if(index==39 && (!read(base+p.sign_mask,signs.data(),sizeof(signs)) || signs!=std::array<std::uint32_t,4>{0x7fffffff,0x7fffffff,0x7fffffff,0x7fffffff}))return false;
    std::array<unsigned char,32> code{};
    const auto scans=command_catalog::scan_code(group.capacity),masks=command_catalog::mask_code(group.mask_offset);
    return read(scan,code.data(),scans.size()) && !std::memcmp(code.data(),scans.data(),scans.size()) &&
        read(mask,code.data(),masks.size()) && !std::memcmp(code.data(),masks.data(),masks.size()) &&
        read(manager+0x20,&current,8) && current==command && read(command,&current,8) && current==table;
}
bool number(const fdkss_value &value,std::uint32_t &raw) {
    if(value.type==FDKSS_INTEGER && value.integer>=-536870912 && value.integer<=536870911) {
        raw=(static_cast<std::uint32_t>(value.integer)&0x3fffffff)|0x40000000;return true;
    }
    if(value.type!=FDKSS_NUMBER || !std::isfinite(value.number) || std::abs(value.number)>std::numeric_limits<float>::max() ||
        (value.number!=0 && std::abs(value.number)<std::numeric_limits<float>::min()))return false;
    const auto narrowed=static_cast<float>(value.number);
    raw=(std::bit_cast<std::uint32_t>(narrowed)>>2)|0x80000000;
    return std::fpclassify(narrowed)!=FP_SUBNORMAL && std::fpclassify(std::bit_cast<float>(raw<<2))!=FP_SUBNORMAL;
}
int select(void *opaque,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size,bool maximum) {
    if(!opaque || !emit || count!=2 || !args)return fail("native min/max expects two finite VM numbers",error,size);
    // Integer-to-float comparisons use the caller's SSE rounding mode. Refuse
    // non-nearest modes rather than changing game-wide floating-point state.
    if(_mm_getcsr()&0x6000)return fail("native min/max requires nearest SSE rounding",error,size);
    const auto &context=*static_cast<const Context *>(opaque);const auto &p=context.second ? second : first;
    std::uintptr_t framework=0,table=0;DWORD owner=0;
    if(!context.image || !read(context.image+p.framework,&framework,8) || !framework ||
        !read(framework,&table,8) || table!=context.image+p.framework_table ||
        !read(framework+p.owner_offset,&owner,4) || owner!=GetCurrentThreadId())
        return fail("native min/max requires the verified update thread",error,size);
    std::uint32_t a=0,b=0;
    if(!number(args[0],a) || !number(args[1],b))return fail("native min/max requires signed 30-bit integers or finite normal/zero VM floats",error,size);
    bool choose_first=false;
    if((a>>30)==1 && (b>>30)==1)choose_first=maximum ? pure_vm::integer(a)>pure_vm::integer(b) : pure_vm::integer(a)<pure_vm::integer(b);
    else {
        const float left=(a>>30)==1 ? static_cast<float>(pure_vm::integer(a)) : std::bit_cast<float>(a<<2);
        const float right=(b>>30)==1 ? static_cast<float>(pure_vm::integer(b)) : std::bit_cast<float>(b<<2);
        choose_first=maximum ? left>right : left<right;
    }
    const auto expected=choose_first ? a : b;
    const auto index=maximum ? 26u : 25u;
    const auto &selected=context.second ? select_second : select_first;
    std::uintptr_t command=0,again=0;
    if(!profile(context,command,index))return fail("native min/max command profile changed",error,size);
    alignas(16) std::array<unsigned char,0x420> storage{};
    const auto stack=reinterpret_cast<std::uintptr_t>(storage.data()+0xc8);
    const std::uint32_t top=8,argc=2;const unsigned char returned=1;
    std::memcpy(storage.data()+0x58,&stack,8);std::memcpy(storage.data()+0x64,&top,4);
    std::memcpy(storage.data()+0x70,&argc,4);std::memcpy(storage.data()+0x80,&returned,1);
    // Native logical argument zero is on top of the VM stack.
    std::memcpy(storage.data()+0xc8,&b,4);std::memcpy(storage.data()+0xcc,&a,4);
    const auto before=storage;
    if(!profile(context,again,index) || again!=command)return fail("native min/max command changed before invocation",error,size);
    using Handler=void (__fastcall *)(void *,void *);
    reinterpret_cast<Handler>(context.image+(maximum ? selected.maximum : selected.minimum))(reinterpret_cast<void *>(command),storage.data());
    std::uint32_t result=0;std::memcpy(&result,storage.data()+0x14,4);
    for(std::size_t i=0;i<storage.size();++i)if((i<0x14 || i>=0x18) && storage[i]!=before[i])
        return fail("native min/max changed private storage outside its return register",error,size);
    if(result!=expected)return fail("native min/max result disagreed with verified scalar selection",error,size);
    fdkss_value value{};
    if((result>>30)==1){value.type=FDKSS_INTEGER;value.integer=pure_vm::integer(result);}
    else {value.type=FDKSS_NUMBER;value.number=std::bit_cast<float>(result<<2);}
    return emit(sink,&value);
}
}
int FDKSS_CALL minimum(void *p,const fdkss_value *a,std::size_t n,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    return select(p,a,n,emit,sink,error,size,false);
}
int FDKSS_CALL maximum(void *p,const fdkss_value *a,std::size_t n,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    return select(p,a,n,emit,sink,error,size,true);
}
bool verified(const Context &context,unsigned index) {
    const auto &p=context.second ? second : first;
    std::uintptr_t framework=0,table=0,command=0;DWORD owner=0;
    return context.image && read(context.image+p.framework,&framework,8) && framework &&
        read(framework,&table,8) && table==context.image+p.framework_table &&
        read(framework+p.owner_offset,&owner,4) && owner==GetCurrentThreadId() &&
        (index==39 || !(_mm_getcsr()&0x6000)) && profile(context,command,index);
}
bool verified_commands(const Context &context,unsigned commands) {
    return !(commands&~15u) && (!(commands&1u) || verified(context,39)) &&
        (!(commands&2u) || verified(context,25)) && (!(commands&4u) || verified(context,26)) &&
        (!(commands&8u) || verified(context,23));
}
bool preview_sqrt(const Context &context,std::uint32_t input,std::uint32_t &output) {
    if((input>>30)!=1 && (input>>30)!=2)return false;
    if((input>>30)==2) {
        const auto bits=input<<2,exponent=bits&0x7f800000u;
        if(exponent==0x7f800000u || (!exponent && (bits&0x7fffffu)))return false;
    }
    if(!verified(context,23))return false;
    const float value=(input>>30)==1 ? static_cast<float>(pure_vm::integer(input)) : std::bit_cast<float>(input<<2);
    if(value<0)return false;
    output=(std::bit_cast<std::uint32_t>(std::sqrt(value))>>2)|0x80000000;return true;
}
int FDKSS_CALL square_root(void *opaque,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(!opaque || !emit || count!=1 || !args)return fail("native_sqrt expects one nonnegative finite VM number",error,size);
    if(_mm_getcsr()&0x6000)return fail("native_sqrt requires nearest SSE rounding",error,size);
    const auto &context=*static_cast<const Context *>(opaque);
    std::uint32_t raw=0,expected=0;
    if(!number(args[0],raw) || !preview_sqrt(context,raw,expected))
        return fail("native_sqrt requires a nonnegative normal/zero VM number and verified owner profile",error,size);
    std::uintptr_t command=0;
    if(!profile(context,command,23))return fail("native_sqrt command profile changed",error,size);
    alignas(16) std::array<unsigned char,0x420> storage{};
    const auto stack=reinterpret_cast<std::uintptr_t>(storage.data()+0xc8);
    const std::uint32_t top=4,argc=1;const unsigned char returned=1;
    std::memcpy(storage.data()+0x58,&stack,8);std::memcpy(storage.data()+0x64,&top,4);
    std::memcpy(storage.data()+0x70,&argc,4);std::memcpy(storage.data()+0x80,&returned,1);
    std::memcpy(storage.data()+0xc8,&raw,4);const auto before=storage;
    const auto &selected=context.second ? select_second : select_first;
    using Handler=void (__fastcall *)(void *,void *);
    reinterpret_cast<Handler>(context.image+selected.square_root)(reinterpret_cast<void *>(command),storage.data());
    std::uint32_t result=0;std::memcpy(&result,storage.data()+0x14,4);
    for(std::size_t i=0;i<storage.size();++i)if((i<0x14 || i>=0x18) && storage[i]!=before[i])
        return fail("native_sqrt changed private storage outside its return register",error,size);
    if(result!=expected)return fail("native_sqrt result disagreed with verified scalar semantics",error,size);
    fdkss_value value{};value.type=FDKSS_NUMBER;value.number=std::bit_cast<float>(result<<2);return emit(sink,&value);
}
bool preview_selection(const Context &context,std::uint32_t a,std::uint32_t b,bool maximum,std::uint32_t &output) {
    if(!verified(context,maximum ? 26u : 25u))return false;
    for(const auto raw:{a,b}) {
        if((raw>>30)!=1 && (raw>>30)!=2)return false;
        if((raw>>30)==2) {
            const auto bits=raw<<2,exponent=bits&0x7f800000u;
            if(exponent==0x7f800000u || (!exponent && (bits&0x7fffffu)))return false;
        }
    }
    bool choose_first=false;
    if((a>>30)==1 && (b>>30)==1)choose_first=maximum ? pure_vm::integer(a)>pure_vm::integer(b) : pure_vm::integer(a)<pure_vm::integer(b);
    else {
        const float left=(a>>30)==1 ? static_cast<float>(pure_vm::integer(a)) : std::bit_cast<float>(a<<2);
        const float right=(b>>30)==1 ? static_cast<float>(pure_vm::integer(b)) : std::bit_cast<float>(b<<2);
        choose_first=maximum ? left>right : left<right;
    }
    output=choose_first ? a : b;return true;
}
bool preview(const Context &context,std::uint32_t input,std::uint32_t &output) {
    if(!pure_vm::scalar(input))return false;
    if((input>>30)==1) {
        const auto value=pure_vm::integer(input);
        if(value==-536870912)return false;
        output=static_cast<std::uint32_t>(value<0 ? -value : value)|0x40000000;
    } else if((input>>30)==2)output=input&~0x20000000u;
    else return false;
    return verified(context);
}
int FDKSS_CALL absolute(void *opaque,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(!opaque || !emit || count!=1 || !args)return fail("native_abs expects one finite VM number",error,size);
    const auto &context=*static_cast<const Context *>(opaque);const auto &p=context.second ? second : first;
    std::uintptr_t framework=0,table=0;DWORD owner=0;
    if(!context.image || !read(context.image+p.framework,&framework,8) || !framework ||
        !read(framework,&table,8) || table!=context.image+p.framework_table ||
        !read(framework+p.owner_offset,&owner,4) || owner!=GetCurrentThreadId())
        return fail("native_abs requires the verified update thread",error,size);
    std::uint32_t raw=0,expected=0;
    if(args[0].type==FDKSS_INTEGER && args[0].integer>=-536870911 && args[0].integer<=536870911) {
        raw=(static_cast<std::uint32_t>(args[0].integer)&0x3fffffff)|0x40000000;
        const auto magnitude=args[0].integer<0 ? -args[0].integer : args[0].integer;
        expected=static_cast<std::uint32_t>(magnitude)|0x40000000;
    } else if(args[0].type==FDKSS_NUMBER && std::isfinite(args[0].number) && std::abs(args[0].number)<=std::numeric_limits<float>::max()) {
        raw=(std::bit_cast<std::uint32_t>(static_cast<float>(args[0].number))>>2)|0x80000000;
        expected=(raw&~0x20000000u)|0x80000000;
    } else return fail("native_abs requires integers within +/-536870911 or finite VM floats",error,size);
    std::uintptr_t command=0;
    if(!profile(context,command))return fail("native_abs command profile changed",error,size);
    // The complete pinned leaf handler reads only these Thread fields and writes
    // register zero. Storage is private; no engine Thread is constructed or queued.
    alignas(16) std::array<unsigned char,0x420> storage{};
    const auto stack=reinterpret_cast<std::uintptr_t>(storage.data()+0xc8);
    const std::uint32_t top=4,argc=1;const unsigned char returned=1;
    std::memcpy(storage.data()+0x58,&stack,8);std::memcpy(storage.data()+0x64,&top,4);
    std::memcpy(storage.data()+0x70,&argc,4);std::memcpy(storage.data()+0x80,&returned,1);
    std::memcpy(storage.data()+0xc8,&raw,4);
    const auto before=storage;
    using Handler=void (__fastcall *)(void *,void *);
    reinterpret_cast<Handler>(context.image+p.handler)(reinterpret_cast<void *>(command),storage.data());
    std::uint32_t result=0;std::memcpy(&result,storage.data()+0x14,4);
    for(std::size_t i=0;i<storage.size();++i)if((i<0x14 || i>=0x18) && storage[i]!=before[i])
        return fail("native_abs changed private storage outside its return register",error,size);
    if(result!=expected || !pure_vm::scalar(result))return fail("native_abs result disagreed with verified scalar semantics",error,size);
    fdkss_value value{};
    if((result>>30)==1){value.type=FDKSS_INTEGER;value.integer=pure_vm::integer(result);}
    else {value.type=FDKSS_NUMBER;value.number=std::bit_cast<float>(result<<2);}
    return emit(sink,&value);
}
}
