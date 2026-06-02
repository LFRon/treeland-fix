add_library(treelandddmremote_qro OBJECT)
set_target_properties(treelandddmremote_qro PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(treelandddmremote_qro PRIVATE Qt6::RemoteObjects)
qt_add_repc_sources(treelandddmremote_qro
    ${CMAKE_CURRENT_SOURCE_DIR}/treelandddmremote.rep
)

target_link_libraries(libtreeland PRIVATE treelandddmremote_qro)
target_include_directories(libtreeland PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/modules/ddm/ddmremoteobjectv1.cpp PROPERTIES SKIP_AUTOMOC ON)

impl_treeland(
    NAME
        module_ddm
    SOURCE
        ${CMAKE_SOURCE_DIR}/src/modules/ddm/ddmremoteobjectv1.cpp
    INCLUDE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)
