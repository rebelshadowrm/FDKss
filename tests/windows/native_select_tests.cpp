#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "native_math.h"
#include "../../src/vm_pure.h"
#include <bit>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <xmmintrin.h>
#include "../fixtures/native_minmax_cases.inc"
#include "../fixtures/native_sqrt_cases.inc"

int main() {
    const auto check=[](bool good,std::source_location at=std::source_location::current()) {
        if(!good)throw std::runtime_error("native selection check at "+std::to_string(at.line()));
    };
    auto *memory=static_cast<unsigned char *>(VirtualAlloc(nullptr,0xc61000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!memory)return 1;
    const auto initial_mxcsr=_mm_getcsr();
    try {
        const auto base=reinterpret_cast<std::uintptr_t>(memory);
        auto put=[](std::uintptr_t at,const auto &value){std::memcpy(reinterpret_cast<void *>(at),&value,sizeof(value));};
        auto code=[](std::uintptr_t at,const auto &value){std::memcpy(reinterpret_cast<void *>(at),value.data(),value.size());};
        auto argument=[](std::uint32_t raw) {
            fdkss_value value{};
            if(raw>>30==1){value.type=FDKSS_INTEGER;value.integer=pure_vm::integer(raw);}
            else {value.type=FDKSS_NUMBER;value.number=std::bit_cast<float>(raw<<2);}
            return value;
        };
        for(const bool second : {false,true}) {
            std::memset(memory,0,0xc61000);native_math::Context context{base,second};
            const auto &p=second ? native_math::second : native_math::first;
            const auto &g=second ? private_timer::second_read : private_timer::first_read;
            const auto &s=second ? native_math::select_second : native_math::select_first;
            const auto framework=base+0x10000,manager=base+0x20000,command=base+0x30000;
            put(base+p.framework,framework);put(framework,base+p.framework_table);put(framework+p.owner_offset,GetCurrentThreadId());
            put(base+g.manager,manager);put(manager,base+(second ? 0xb12fd0 : 0x7fc400));put(manager+0x20,command);put(command,base+g.table);
            put(base+g.table+16,base+g.scan);put(base+g.table+24,base+g.mask);
            code(base+g.scan,command_catalog::scan_code(g.capacity));code(base+g.mask,command_catalog::mask_code(g.mask_offset));
            for(const bool maximum : {false,true}) {
                const auto index=maximum ? 26u : 25u;
                const auto offset=maximum ? s.maximum : s.minimum;
                const auto hex=maximum ? native_math::max_code : native_math::min_code;
                const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
                for(unsigned i=0;i<280;++i)memory[offset+i]=static_cast<unsigned char>(nibble(hex[2*i])*16+nibble(hex[2*i+1]));
                put(command+8+index*24,base+offset);put(command+g.mask_offset+index*4,std::uint32_t{1});
                fdkss_value args[2]{},result{};char error[256]{};
                auto capture=+[](void *out,const fdkss_value *value)->int{*static_cast<fdkss_value *>(out)=*value;return 0;};
                const auto binding=maximum ? native_math::maximum : native_math::minimum;
                auto invoke=[&](std::size_t count=2){return binding(&context,args,count,capture,&result,error,sizeof(error));};
                for(const auto &row:selection_cases) {
                    std::uint32_t predicted=0;
                    check(native_math::preview_selection(context,row[0],row[1],maximum,predicted) && predicted==row[maximum ? 3 : 2]);
                    const unsigned char bytecode[]{36,0,static_cast<unsigned char>(index),2,1,8,13};
                    const auto preview=+[](void *opaque,unsigned group,unsigned command,const std::uint32_t *args,unsigned count,std::uint32_t &out)->bool {
                        return group==0 && (command==25 || command==26) && count==2 && args &&
                            native_math::preview_selection(*static_cast<native_math::Context *>(opaque),args[1],args[0],command==26,out);
                    };
                    check(pure_vm::evaluate(bytecode,sizeof(bytecode),row,2,predicted,0,nullptr,&context,0,nullptr,nullptr,preview) &&
                        predicted==row[maximum ? 3 : 2]);
                    args[0]=argument(row[0]);args[1]=argument(row[1]);check(!invoke());
                    const auto raw=result.type==FDKSS_INTEGER ? (static_cast<std::uint32_t>(result.integer)&0x3fffffff)|0x40000000 :
                        (std::bit_cast<std::uint32_t>(static_cast<float>(result.number))>>2)|0x80000000;
                    check(raw==row[maximum ? 3 : 2]);
                }
                for(const auto bad:{0u,0xc0000042u,0x9ff00000u,0x80000001u}) {
                    std::uint32_t output=0;
                    check(!native_math::preview_selection(context,bad,0x40000001,maximum,output));
                    check(!native_math::preview_selection(context,0x40000001,bad,maximum,output));
                }
                args[0]=argument(0x40000001);args[1]=argument(0x40000002);
                check(invoke(0)!=0 && invoke(1)!=0 && invoke(3)!=0);
                for(unsigned which=0;which<2;++which) {
                    for(const auto tag:{FDKSS_NIL,FDKSS_STRING,FDKSS_BOOLEAN}) {
                        const auto before=args[which];args[which].type=tag;check(invoke()!=0);args[which]=before;
                    }
                    const auto before=args[which];args[which].type=FDKSS_INTEGER;
                    for(const auto value:{INT64_MIN,INT64_MAX,std::int64_t{-536870913},std::int64_t{536870912}}) {
                        args[which].integer=value;check(invoke()!=0);
                    }
                    args[which].type=FDKSS_NUMBER;
                    for(const auto value:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::max(),static_cast<double>(std::numeric_limits<float>::denorm_min()),1e-300}) {
                        args[which].number=value;check(invoke()!=0);
                    }
                    args[which]=before;
                }
                bool foreign=false;std::thread worker([&]{foreign=invoke()!=0;});worker.join();check(foreign);
                put(command+8+index*24+8,std::uint64_t{1});check(invoke()!=0);put(command+8+index*24+8,std::uint64_t{0});
                put(command+8+index*24+16,std::uint32_t{1});check(invoke()!=0);put(command+8+index*24+16,std::uint32_t{0});
                put(command+g.mask_offset+index*4,std::uint32_t{0});check(invoke()!=0);put(command+g.mask_offset+index*4,std::uint32_t{1});
                for(const auto at:{offset,offset+279,g.scan,g.mask}) {memory[at]^=1;check(invoke()!=0);memory[at]^=1;}
                put(command+8+index*24,base+offset+1);check(invoke()!=0);put(command+8+index*24,base+offset);
                put(command,base+g.table+8);check(invoke()!=0);put(command,base+g.table);
                put(manager,base+(second ? 0xb12fd0 : 0x7fc400)+8);check(invoke()!=0);put(manager,base+(second ? 0xb12fd0 : 0x7fc400));
                put(framework,base+p.framework_table+8);check(invoke()!=0);put(framework,base+p.framework_table);
                put(manager+0x20,std::uintptr_t{0});check(invoke()!=0);put(manager+0x20,command);
                const auto saved=_mm_getcsr();_mm_setcsr((saved&~0x6000u)|0x2000u);check(invoke()!=0);_mm_setcsr(saved);
                DWORD previous=0,ignored=0;check(VirtualProtect(memory+offset,280,PAGE_READWRITE,&previous));check(invoke()!=0);
                check(VirtualProtect(memory+offset,280,previous,&ignored));check(!invoke());
            }
            const auto root=s.square_root;
            const auto root_hex=second ? native_math::sqrt_sky2_code : native_math::sqrt_sky1_code;
            const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
            for(unsigned i=0;i<138;++i)memory[root+i]=static_cast<unsigned char>(nibble(root_hex[2*i])*16+nibble(root_hex[2*i+1]));
            put(command+8+23*24,base+root);put(command+g.mask_offset+23*4,std::uint32_t{1});
            fdkss_value input{},output{};char error[256]{};
            const auto capture=+[](void *sink,const fdkss_value *value)->int{*static_cast<fdkss_value *>(sink)=*value;return 0;};
            const auto invoke=[&](std::size_t count=1){return native_math::square_root(&context,&input,count,capture,&output,error,sizeof(error));};
            const auto preview=+[](void *opaque,unsigned group,unsigned command,const std::uint32_t *args,unsigned count,std::uint32_t &out)->bool {
                return group==0 && command==23 && count==1 && args &&
                    native_math::preview_sqrt(*static_cast<native_math::Context *>(opaque),args[0],out);
            };
            constexpr unsigned char root_code[]{36,0,23,1,1,4,13};
            for(const auto &row:sqrt_cases) {
                std::uint32_t predicted=0;
                check(native_math::preview_sqrt(context,row[0],predicted) && predicted==row[1]);
                check(pure_vm::evaluate(root_code,sizeof(root_code),row,1,predicted,0,nullptr,&context,0,nullptr,nullptr,preview) && predicted==row[1]);
                input=argument(row[0]);check(!invoke() && output.type==FDKSS_NUMBER);
                check(((std::bit_cast<std::uint32_t>(static_cast<float>(output.number))>>2)|0x80000000)==row[1]);
            }
            for(const auto bad:{0u,0xc0000042u,0x9ff00000u,0x80000001u,0x7fffffffu,0xafc00000u}) {
                std::uint32_t predicted=0;check(!native_math::preview_sqrt(context,bad,predicted));
            }
            for(const auto bad:{argument(0x7fffffff),argument(0xafc00000)}){input=bad;check(invoke()!=0);}
            for(const auto tag:{FDKSS_NIL,FDKSS_STRING,FDKSS_BOOLEAN}){input={};input.type=tag;check(invoke()!=0);}
            input.type=FDKSS_NUMBER;
            for(const auto value:{-1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::max(),1e-40,1e-300}){input.number=value;check(invoke()!=0);}
            input=argument(0x40000004);check(invoke(0)!=0 && invoke(2)!=0);
            bool foreign=false;std::thread worker([&]{foreign=invoke()!=0;});worker.join();check(foreign);
            for(const auto at:{root,root+137,g.scan,g.mask}){memory[at]^=1;check(invoke()!=0);memory[at]^=1;}
            put(command+8+23*24+8,std::uint64_t{1});check(invoke()!=0);put(command+8+23*24+8,std::uint64_t{0});
            put(command+8+23*24+16,std::uint32_t{1});check(invoke()!=0);put(command+8+23*24+16,std::uint32_t{0});
            put(command+g.mask_offset+23*4,std::uint32_t{0});check(invoke()!=0);put(command+g.mask_offset+23*4,std::uint32_t{1});
            const auto saved=_mm_getcsr();_mm_setcsr((saved&~0x6000u)|0x2000u);check(invoke()!=0);_mm_setcsr(saved);
            DWORD previous=0,ignored=0;check(VirtualProtect(memory+root,138,PAGE_READWRITE,&previous));check(invoke()!=0);
            check(VirtualProtect(memory+root,138,previous,&ignored));check(!invoke());
        }
        _mm_setcsr(initial_mxcsr);VirtualFree(memory,0,MEM_RELEASE);
        std::cout<<"Native min/max original CPU fixtures and refusal guards passed\n";
    }catch(const std::exception &error){_mm_setcsr(initial_mxcsr);VirtualFree(memory,0,MEM_RELEASE);std::cerr<<error.what()<<'\n';return 1;}
}
