#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "native_math.h"
#include "../../src/vm_pure.h"
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
int main() {
    auto check=[](bool good,std::source_location location=std::source_location::current()) {
        if(!good)throw std::runtime_error("native math check at "+std::to_string(location.line()));
    };
    auto *memory=static_cast<unsigned char *>(VirtualAlloc(nullptr,0xc61000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!memory)return 1;
    try {
        const auto base=reinterpret_cast<std::uintptr_t>(memory);
        auto put=[](std::uintptr_t at,const auto &value){std::memcpy(reinterpret_cast<void *>(at),&value,sizeof(value));};
        auto code=[](std::uintptr_t at,const auto &value){std::memcpy(reinterpret_cast<void *>(at),value.data(),value.size());};
        for(const bool second : {false,true}) {
            std::memset(memory,0,0xc61000);native_math::Context context{base,second};
            const auto &p=second ? native_math::second : native_math::first;
            const auto &g=second ? private_timer::second_read : private_timer::first_read;
            const auto framework=base+0x10000,manager=base+0x20000,command=base+0x30000;
            put(base+p.framework,framework);put(framework,base+p.framework_table);put(framework+p.owner_offset,GetCurrentThreadId());
            put(base+g.manager,manager);put(manager,base+(second ? 0xb12fd0 : 0x7fc400));put(manager+0x20,command);put(command,base+g.table);
            put(base+g.table+16,base+g.scan);put(base+g.table+24,base+g.mask);
            put(command+8+39*24,base+p.handler);put(command+g.mask_offset+39*4,std::uint32_t{1});
            put(command+8+39*24+16,std::uint64_t{0xffffffff00000000ull});
            code(base+g.scan,command_catalog::scan_code(g.capacity));code(base+g.mask,command_catalog::mask_code(g.mask_offset));
            const auto hex=second ? native_math::second_code : native_math::first_code;
            auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
            for(unsigned i=0;i<139;++i)memory[p.handler+i]=static_cast<unsigned char>(nibble(hex[2*i])*16+nibble(hex[2*i+1]));
            put(base+p.sign_mask,std::array<std::uint32_t,4>{0x7fffffff,0x7fffffff,0x7fffffff,0x7fffffff});
            fdkss_value argument{},result{};char error[256]{};
            auto capture=+[](void *out,const fdkss_value *value)->int{*static_cast<fdkss_value *>(out)=*value;return 0;};
            auto invoke=[&](std::size_t count=1){return native_math::absolute(&context,&argument,count,capture,&result,error,sizeof(error));};
            auto predicted=+[](void *context,unsigned group,unsigned command,const std::uint32_t *args,unsigned count,std::uint32_t &out)->bool {
                return group==0 && command==39 && count==1 && args && native_math::preview(*static_cast<native_math::Context *>(context),args[0],out);
            };
            constexpr unsigned char bytecode[]{36,0,39,1,1,4,13};
            const std::uint32_t encoded_negative=0x7fffffedu;std::uint32_t encoded_result=0;
            check(pure_vm::evaluate(bytecode,sizeof(bytecode),&encoded_negative,1,encoded_result,0,nullptr,&context,0,nullptr,nullptr,predicted) && encoded_result==0x40000013);
            for(const auto bad : {0u,0xc0000042u,0x60000000u,0x9ff00000u})check(!native_math::preview(context,bad,encoded_result));
            argument.type=FDKSS_INTEGER;
            for(const std::int64_t value : {-536870911LL,-19LL,-1LL,0LL,1LL,19LL,536870911LL}) {
                argument.integer=value;check(!invoke() && result.type==FDKSS_INTEGER && result.integer==std::abs(value));
            }
            for(const std::int64_t value : {-536870912LL,536870912LL,INT64_MIN,INT64_MAX}) {argument.integer=value;check(invoke()!=0);}
            argument.type=FDKSS_NUMBER;
            for(const double value : {-1.25,-0.0,0.0,1.25,-1234.12345,static_cast<double>(std::numeric_limits<float>::max()),-static_cast<double>(std::numeric_limits<float>::max())}) {
                argument.number=value;const auto bits=std::bit_cast<std::uint32_t>(static_cast<float>(value))&0x7ffffffcu;
                check(!invoke() && result.type==FDKSS_NUMBER && result.number==std::bit_cast<float>(bits) && !std::signbit(result.number));
            }
            for(const double value : {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),std::numeric_limits<double>::max()}) {argument.number=value;check(invoke()!=0);}
            for(const auto tag : {FDKSS_NIL,FDKSS_STRING,FDKSS_BOOLEAN}){argument.type=tag;check(invoke()!=0);}
            argument.type=FDKSS_INTEGER;argument.integer=-3;check(invoke(0)!=0 && invoke(2)!=0);
            bool foreign=false;std::thread thread([&]{foreign=invoke()!=0;});thread.join();check(foreign);
            put(framework,base+p.framework_table+8);check(invoke()!=0);put(framework,base+p.framework_table);
            put(command,base+g.table+8);check(invoke()!=0);put(command,base+g.table);
            put(manager,base+(second ? 0xb12fd0 : 0x7fc400)+8);check(invoke()!=0);put(manager,base+(second ? 0xb12fd0 : 0x7fc400));
            put(command+8+39*24+8,std::uint64_t{1});check(invoke()!=0);put(command+8+39*24+8,std::uint64_t{0});
            put(command+8+39*24+16,std::uint64_t{1});check(invoke()!=0);put(command+8+39*24+16,std::uint64_t{0});
            put(command+g.mask_offset+39*4,std::uint32_t{0});check(invoke()!=0);put(command+g.mask_offset+39*4,std::uint32_t{1});
            for(const auto at : {p.handler,p.handler+138,p.sign_mask,g.scan,g.mask}) {memory[at]^=1;check(invoke()!=0);memory[at]^=1;}
            put(manager+0x20,std::uintptr_t{0});check(invoke()!=0);put(manager+0x20,command);
            DWORD previous=0,ignored=0;check(VirtualProtect(memory+p.handler,139,PAGE_READWRITE,&previous));check(invoke()!=0);
            check(VirtualProtect(memory+p.handler,139,previous,&ignored));check(!invoke() && result.integer==3);
        }
        VirtualFree(memory,0,MEM_RELEASE);std::cout<<"Native math tests passed\n";
    }catch(const std::exception &error){VirtualFree(memory,0,MEM_RELEASE);std::cerr<<error.what()<<'\n';return 1;}
}
