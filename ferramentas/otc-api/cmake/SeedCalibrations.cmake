# Seed packaged profiles, but never overwrite live calibration from this DLL.
file(MAKE_DIRECTORY "${DEST_DIR}")
file(GLOB profiles "${SOURCE_DIR}/pe-*.json")
foreach(profile IN LISTS profiles)
    get_filename_component(name "${profile}" NAME)
    if(NOT EXISTS "${DEST_DIR}/${name}")
        configure_file("${profile}" "${DEST_DIR}/${name}" COPYONLY)
    endif()
endforeach()

