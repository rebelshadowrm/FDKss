// Independently verified Sky 2 build d8b2911d properties.
FDKSS_PROPERTY(framework, run_in_background, 0x0c, FDKSS_BOOLEAN, bool, false)
// Framework update 5BC5F0, fed from global C60EC0 by 7C869E/7C885E.
// The one-second frame counter is converted and stored at 5BC6A3.
FDKSS_PROPERTY(framework, fps, 0x30, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(framework, delta_seconds, 0x38, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(framework, time_scale, 0x40, FDKSS_NUMBER, float, true)
FDKSS_PROPERTY(framework, update_thread, 0xa0, FDKSS_INTEGER, uint32_t, false)
FDKSS_PROPERTY(scene, map_id, 0x30, FDKSS_STRING, char32, false)
FDKSS_PROPERTY(script_manager, program_count, 0x10, FDKSS_INTEGER, uint64_t, false)
// Current player's Model; independently traced MDL header and vector counts.
FDKSS_PROPERTY(player_model, name, 0x1c, FDKSS_STRING, char64, false)
FDKSS_PROPERTY(player_model, format_version, 0x14, FDKSS_INTEGER, uint32_t, false)
FDKSS_PROPERTY(player_model, mesh_count, 0x258, FDKSS_INTEGER, uint64_t, false)
FDKSS_PROPERTY(player_model, node_count, 0x288, FDKSS_INTEGER, uint64_t, false)
FDKSS_PROPERTY(actor, name, 0x10, FDKSS_STRING, char32, false)
FDKSS_PROPERTY(actor, entity_id, 0x38, FDKSS_INTEGER, uint32_t, false)

// Inherited by actors with a nonvirtual Actor@fdk base at offset zero.
// Engine matrix memory order; translation occupies m30, m31, m32.
FDKSS_PROPERTY(actor, transform_m00, 0x58, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m01, 0x5c, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m02, 0x60, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m03, 0x64, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m10, 0x68, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m11, 0x6c, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m12, 0x70, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m13, 0x74, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m20, 0x78, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m21, 0x7c, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m22, 0x80, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m23, 0x84, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m30, 0x88, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m31, 0x8c, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m32, 0x90, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, transform_m33, 0x94, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, position_x, 0xd8, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, position_y, 0xdc, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, position_z, 0xe0, FDKSS_NUMBER, float, false)

FDKSS_PROPERTY(player, position_x, 0xd8, FDKSS_NUMBER, float, true)
FDKSS_PROPERTY(player, position_y, 0xdc, FDKSS_NUMBER, float, true)
FDKSS_PROPERTY(player, position_z, 0xe0, FDKSS_NUMBER, float, true)
// Computed from the independently verified quaternion; setter uses Sky 2 placement.
FDKSS_PROPERTY(player, yaw_radians, 0x108, FDKSS_NUMBER, computed_yaw, true)
FDKSS_PROPERTY(player, native_state_byte, 0x16c, FDKSS_INTEGER, uint8_t, true)

FDKSS_PROPERTY(actor, rotation_x, 0x108, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, rotation_y, 0x10c, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, rotation_z, 0x110, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, rotation_w, 0x114, FDKSS_NUMBER, float, false)
// Local diagonal scale in the Actor+58 transform block; not decomposed world scale.
FDKSS_PROPERTY(actor, scale_x, 0x138, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, scale_y, 0x13c, FDKSS_NUMBER, float, false)
FDKSS_PROPERTY(actor, scale_z, 0x140, FDKSS_NUMBER, float, false)
// Raw native state; individual bit/byte meanings are not exposed as semantic flags.
FDKSS_PROPERTY(actor, native_state_byte, 0x16c, FDKSS_INTEGER, uint8_t, false)
FDKSS_PROPERTY(actor, native_state_flags, 0x170, FDKSS_INTEGER, uint32_t, false)

// Copied derived resources via current status links; no embedded Chara fields.
FDKSS_PROPERTY(player, hp, 0x1d90, FDKSS_INTEGER, computed_resource, true)
FDKSS_PROPERTY(player, max_hp, 0x1d90, FDKSS_INTEGER, computed_resource, false)
FDKSS_PROPERTY(player, ep, 0x1d90, FDKSS_INTEGER, computed_resource, true)
FDKSS_PROPERTY(player, max_ep, 0x1d90, FDKSS_INTEGER, computed_resource, false)
FDKSS_PROPERTY(player, cp, 0x1d90, FDKSS_INTEGER, computed_resource, true)
FDKSS_PROPERTY(player, max_cp, 0x1d90, FDKSS_INTEGER, computed_resource, false)
