# Copyright (C) 2026 UnionTech Software Technology Co., Ltd.
# SPDX-License-Identifier: Apache-2.0 OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick Test)

set(TEST_DCONFIG_DATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/share/dsg")
file(COPY "${CMAKE_SOURCE_DIR}/misc/dconfig/"
    DESTINATION "${TEST_DCONFIG_DATA_DIR}/configs/org.deepin.dde.treeland"
    FILES_MATCHING
        PATTERN "*.json"
)
find_file(DTK_PREFERENCE_DCONFIG
    NAMES org.deepin.dtk.preference.json
    PATH_SUFFIXES dsg/configs
)
if(DTK_PREFERENCE_DCONFIG)
    file(COPY "${DTK_PREFERENCE_DCONFIG}"
        DESTINATION "${TEST_DCONFIG_DATA_DIR}/configs"
    )
endif()

add_executable(test_dtk_inwindowblur_override
    main.cpp
)

target_compile_definitions(test_dtk_inwindowblur_override
    PRIVATE
        TREELAND_QML_IMPORT_PATH="${PROJECT_BINARY_DIR}/qt/qml"
        WAYLIB_QML_IMPORT_PATH="${PROJECT_BINARY_DIR}/waylib/src/server"
)

target_link_libraries(test_dtk_inwindowblur_override
    PRIVATE
        libtreeland
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::Test
)

add_test(NAME test_dtk_inwindowblur_override COMMAND test_dtk_inwindowblur_override)
set_tests_properties(test_dtk_inwindowblur_override PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;DSG_DCONFIG_BACKEND_TYPE=FileBackend;DSG_DATA_DIRS=${TEST_DCONFIG_DATA_DIR};XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config"
    TIMEOUT 30
)
