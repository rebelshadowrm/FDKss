#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "command_objects.h"
#include <array>
#include <stdexcept>
#include <thread>
#include <iostream>
#include <source_location>
#include <string>
int main() {
    auto check=[](bool b,std::source_location where=std::source_location::current()){if(!b)throw std::runtime_error("command object check at "+std::to_string(where.line()));};
    auto *memory=static_cast<unsigned char *>(VirtualAlloc(nullptr,0xc70000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    if(!memory)return 1;
    try {
        const auto image=reinterpret_cast<std::uintptr_t>(memory);
        auto put=[&](std::uintptr_t address,const auto &value){std::memcpy(reinterpret_cast<void *>(address),&value,sizeof(value));};
        auto code=[&](std::uintptr_t address,const auto &bytes){std::memcpy(reinterpret_cast<void *>(address),bytes.data(),bytes.size());};
        IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x100;put(image,dos);
        IMAGE_NT_HEADERS64 pe{};pe.Signature=IMAGE_NT_SIGNATURE;pe.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;pe.OptionalHeader.SizeOfImage=0xc70000;put(image+0x100,pe);
        for(bool second : {false,true}) {
            const auto &profile=(second ? command_objects::second : command_objects::first)[0];
            const auto framework=image+0x10000,manager=image+0x20000,object=image+0x30000,handler=image+0x40000;
            put(image+(second ? 0xc60ec0 : 0x8ff030),framework);put(framework,image+(second ? 0xb1ad08 : 0x802d80));
            put(framework+(second ? 0xa0 : 0x68),GetCurrentThreadId());
            const auto manager_slot=image+(second ? 0xc60e58 : 0x8fefc8);
            put(manager_slot,manager);put(manager,image+(second ? 0xb12fd0 : 0x7fc400));put(manager+0x20,object);
            std::memset(reinterpret_cast<void *>(object),0,4096);put(object,image+profile.table);
            put(image+profile.table+16,image+profile.scan);put(image+profile.table+24,image+profile.mask);
            code(image+profile.scan,command_catalog::scan_code(profile.capacity));code(image+profile.mask,command_catalog::mask_code(profile.mask_offset));
            put(object+8,handler);put(object+8+2*24,handler);put(object+profile.mask_offset,std::uint32_t{1});
            command_objects::Object row;
            auto resolve=[&](std::string_view root="script_group:0"){return command_objects::resolve(image,second,root,row);};
            check(resolve() && row.address==object && row.group==0 && row.slots==profile.capacity && row.implemented==2 && std::string_view(row.type)==profile.type);
            for(const auto bad : {"script_group:","script_group:-1","script_group:28","script_group:00","script_group:0junk","script_group:99999999999999999999","player"})check(!resolve(bad) && !row.address);
            bool foreign=false;std::thread thread([&]{command_objects::Object copy;foreign=!command_objects::resolve(image,second,"script_group:0",copy);});thread.join();check(foreign);
            const auto replacement=image+0x35000;std::memcpy(reinterpret_cast<void *>(replacement),reinterpret_cast<void *>(object),4096);
            put(manager+0x20,replacement);check(resolve() && row.address==replacement && row.implemented==2);put(manager+0x20,object);
            put(manager+0x20,std::uintptr_t{0});check(!resolve());put(manager+0x20,object);
            put(manager_slot,std::uintptr_t{0});check(!resolve());put(manager_slot,manager);
            put(object,image+profile.table+8);check(!resolve());put(object,image+profile.table);
            memory[profile.scan]^=1;check(!resolve());memory[profile.scan]^=1;
            put(object+8,image+0xc70000);check(!resolve());put(object+8,handler);
            DWORD previous=0;check(VirtualProtect(reinterpret_cast<void *>(handler),4096,PAGE_READWRITE,&previous));check(!resolve());DWORD ignored=0;check(VirtualProtect(reinterpret_cast<void *>(handler),4096,previous,&ignored));
            fdkss_value args[2]{};args[0].type=args[1].type=FDKSS_STRING;args[0].string="script_group:0";args[0].string_size=14;args[1].string="implemented_count";args[1].string_size=17;
            auto capture=+[](void *out,const fdkss_value *value)->int{*static_cast<std::int64_t *>(out)=value->integer;return 0;};
            char error[256]{};std::int64_t value=-1;check(!command_objects::get(image,second,args,2,capture,&value,error,sizeof(error)) && value==2);
            args[1].string="write";args[1].string_size=5;check(command_objects::get(image,second,args,2,capture,&value,error,sizeof(error))!=0);
        }
        VirtualFree(memory,0,MEM_RELEASE);std::cout<<"Command object checks passed\n";
    }catch(const std::exception &e){VirtualFree(memory,0,MEM_RELEASE);std::cerr<<e.what()<<'\n';return 1;}
}
