#pragma once
#include "fdkss/api.h"
#include "../vm_timer_read.h"
namespace native_math {
struct Context { std::uintptr_t image; bool second; };
struct Profile { std::uintptr_t handler, sign_mask, framework, framework_table; unsigned owner_offset; };
inline constexpr Profile first{0x3d7620,0x817cf0,0x8ff030,0x802d80,0x68};
inline constexpr Profile second{0x4c4510,0xb30f70,0xc60ec0,0xb1ad08,0xa0};
inline constexpr char first_code[]="837a70007707b800000040eb1648634a64488b42588b4401fc8bc8c1e91e83f90175288d048500000000c1f8028bc8f7d90f48c881e1ffffff3f0fbae91e80ba80000000007443894a14c3440fb6828000000083f90275268d048500000000660f6ec00f540566064400660f7ec0c1e8020fbae81f4584c07410894214c34584c07407c7421400000040c3";
inline constexpr char second_code[]="837a70007707b800000040eb1648634a64488b42588b4401fc8bc8c1e91e83f90175288d048500000000c1f8028bc8f7d90f48c881e1ffffff3f0fbae91e80ba80000000007443894a14c3440fb6828000000083f90275268d048500000000660f6ec00f5405f6c96600660f7ec0c1e8020fbae81f4584c07410894214c34584c07407c7421400000040c3";
#include "native_select_profiles.inc"
#include "native_sqrt_profiles.inc"
struct SelectProfile {std::uintptr_t minimum,maximum,square_root;};
inline constexpr SelectProfile select_first{0x3d7310,0x3d7430,0x3d71c0};
inline constexpr SelectProfile select_second{0x4c4200,0x4c4320,0x4c40b0};
int FDKSS_CALL absolute(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
int FDKSS_CALL minimum(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
int FDKSS_CALL maximum(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
int FDKSS_CALL square_root(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
bool verified(const Context &,unsigned command=39);
bool verified_commands(const Context &,unsigned commands);
bool preview(const Context &,std::uint32_t,std::uint32_t &);
bool preview_selection(const Context &,std::uint32_t,std::uint32_t,bool maximum,std::uint32_t &);
bool preview_sqrt(const Context &,std::uint32_t,std::uint32_t &);
}
