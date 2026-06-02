set(TREELAND_DDM_REMOTE_SOURCE_HEADER
    ${CMAKE_CURRENT_BINARY_DIR}/rep_treelandddmremote_source.h
)
add_custom_command(
    OUTPUT ${TREELAND_DDM_REMOTE_SOURCE_HEADER}
    COMMAND $<TARGET_FILE:Qt6::repc> -o source
            ${CMAKE_CURRENT_SOURCE_DIR}/treelandddmremote.rep
            ${TREELAND_DDM_REMOTE_SOURCE_HEADER}
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/treelandddmremote.rep
    DEPENDS Qt6::repc
    VERBATIM
)
set_source_files_properties(
    ${TREELAND_DDM_REMOTE_SOURCE_HEADER}
    PROPERTIES GENERATED TRUE
)
add_custom_target(generate_treeland_ddm_remote_source
    DEPENDS ${TREELAND_DDM_REMOTE_SOURCE_HEADER}
)

impl_treeland(
    NAME
        module_ddm
    SOURCE
        ${TREELAND_DDM_REMOTE_SOURCE_HEADER}
        ${CMAKE_SOURCE_DIR}/src/modules/ddm/ddminterfacev1.cpp
    INCLUDE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
add_dependencies(libtreeland generate_treeland_ddm_remote_source)
