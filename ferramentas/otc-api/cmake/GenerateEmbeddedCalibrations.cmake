if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "SOURCE_DIR and OUTPUT_FILE are required")
endif()

file(GLOB profiles "${SOURCE_DIR}/pe-*.json")
list(SORT profiles)

get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT_FILE}"
    "#include \"otc_api/embedded_calibrations.h\"\n\n"
    "namespace otc {\nnamespace {\n")

set(index 0)
foreach(profile IN LISTS profiles)
    get_filename_component(build_id "${profile}" NAME_WE)
    file(READ "${profile}" profile_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," profile_bytes "${profile_hex}")
    file(APPEND "${OUTPUT_FILE}"
        "static const unsigned char profile_${index}[] = {${profile_bytes}};\n")
    math(EXPR index "${index} + 1")
endforeach()

file(APPEND "${OUTPUT_FILE}" "}\n\nstd::string_view embeddedCalibrationJson(std::string_view buildId) {\n")
set(index 0)
foreach(profile IN LISTS profiles)
    get_filename_component(build_id "${profile}" NAME_WE)
    file(APPEND "${OUTPUT_FILE}"
        "    if (buildId == \"${build_id}\")\n"
        "        return {reinterpret_cast<const char*>(profile_${index}), sizeof(profile_${index})};\n")
    math(EXPR index "${index} + 1")
endforeach()
file(APPEND "${OUTPUT_FILE}" "    return {};\n}\n\n} // namespace otc\n")

