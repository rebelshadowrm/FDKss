file(SHA256 "${HOST}" HOST_HASH)
get_filename_component(OUT_DIRECTORY "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OUT_DIRECTORY}")
file(WRITE "${OUT}" "#define FDKSS_TEST_SHA256 \"${HOST_HASH}\"\n")
