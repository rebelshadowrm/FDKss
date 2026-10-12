#pragma once
#include "player_resources.h"
namespace resource_properties {
inline int index(std::string_view name) {
    constexpr std::array<std::string_view,6> names{"hp","max_hp","ep","max_ep","cp","max_cp"};
    for(unsigned i=0;i<names.size();++i)if(names[i]==name)return static_cast<int>(i);
    return -1;
}
template<class Reader> int get(Reader reader,const fdkss_value *args,unsigned selected,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    struct Copy {std::array<fdkss_value,6> values{};unsigned count=0;};Copy copied;
    const auto capture=+[](void *opaque,const fdkss_value *value)->int{
        auto &copy=*static_cast<Copy *>(opaque);if(copy.count>=copy.values.size())return 1;
        copy.values[copy.count++]=*value;return 0;
    };
    if(reader(nullptr,args,1,capture,&copied,error,capacity))return 1;
    if(copied.count==1 && copied.values[0].type==FDKSS_NIL)return emit(sink,&copied.values[0]);
    bool valid=copied.count==6 && selected<6;
    for(unsigned i=0;i<copied.count;++i)valid=valid && copied.values[i].type==FDKSS_INTEGER;
    if(!valid){std::snprintf(error,capacity,"invalid copied character resource result");return 1;}
    return emit(sink,&copied.values[selected]);
}
template<class Resolve> int set(std::uintptr_t image,const player_resources::Profile &profile,
    const player_resources::SetterProfile &setter,Resolve resolve,const fdkss_value *args,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity,
    player_resources::Setter (*checked_target)(std::uintptr_t,const player_resources::SetterProfile &)=nullptr) {
    const std::string_view reference(args[0].string,args[0].string_size);
    const auto selected=index(std::string_view(args[1].string,args[1].string_size));
    if(reference!="player" && (reference.empty() || reference.front()!='@')) {
        std::snprintf(error,capacity,"resource editing requires player or a scene character reference");return 1;
    }
    if(selected<0 || selected%2) {std::snprintf(error,capacity,"resource maxima are read-only");return 1;}
    return player_resources::edit(image,profile,setter,[&]{return resolve(reference);},args+1,2,emit,sink,error,capacity,checked_target);
}
}
