#pragma once
#include <cstddef>
#include "fdkss/api.h"
namespace sky1 {
int FDKSS_CALL actor_model(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL character_model(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL character_status_value(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL character_resources(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL character_conditions(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL player_conditions(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL set_player_resource(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL player_resources(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL battle_roster(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL class_info(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
// These layouts apply only to the SHA-256-pinned 1.04.2 profile in attach.cpp.
void **update_slot();
int describe(const char *kind, char *output, std::size_t capacity);
int set_time_scale(const char *input, char *output, std::size_t capacity);
bool restore_changes();
int FDKSS_CALL get_property(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL set_property(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL read_resource(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL list_objects(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
void refresh_scene();
bool start_lifetimes();
void emit_lifetime_events(fdkss_runtime *);
int FDKSS_CALL call_method(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
}
