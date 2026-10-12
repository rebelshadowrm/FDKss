#pragma once
#include "fdkss/api.h"
#include <cstddef>
namespace sky2 {
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
int FDKSS_CALL read_resource(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL content_self_test(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
// Independent layout for the SHA-256-pinned Sky 2 executable in attach.cpp.
void **update_slot();
bool owner_thread();
// Copied JSON diagnostics for existing verified current roots; addresses are not handles.
int describe(const char *kind, char *buffer, std::size_t capacity);
bool start_lifetimes();
void refresh_scene();
void emit_lifetime_events(fdkss_runtime *);
bool restore_changes();
int FDKSS_CALL get_property(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL set_property(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL call_method(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL list_objects(void *, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
}
