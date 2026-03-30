add_library(gui-qt STATIC
    src/main_window.cpp
    src/about_dialog.cpp
    src/log_widget.cpp
    src/game_window.cpp
    src/game_compatibility.cpp
    src/game_list_delegate.cpp
    src/game_list_table.cpp
    src/game_list.cpp
    src/game_list_context_menu.cpp
    src/ctrl_keyboard_filter.cpp
    src/trophy_notifier.cpp
    src/trophy_toast.cpp
    src/trophy_collection_dialog.cpp
    src/game_overlay_base.cpp
    src/game_overlay.cpp
    src/common_dialog_overlay.cpp
    src/ime_overlay.cpp
    src/shader_hint_overlay.cpp
    src/perf_overlay.cpp
    src/firmware_install_dialog.cpp
    src/pkg_install_dialog.cpp
    src/license_install_dialog.cpp
    src/archive_install_dialog.cpp
    src/user_management_dialog.cpp
    src/controls_dialog.cpp
    src/settings_dialog.cpp
    src/welcome_dialog.cpp
    src/qt_utils.cpp
    src/gui_settings_base.cpp
    src/gui_settings.cpp
    src/persistent_settings.cpp

    src/main_window.ui
    src/about_dialog.ui
    src/controls_dialog.ui
    src/settings_dialog.ui
    src/welcome_dialog.ui

    include/gui-qt/main_window.h
    include/gui-qt/about_dialog.h
    include/gui-qt/log_widget.h
    include/gui-qt/custom_dock_widget.h
    include/gui-qt/game_window.h
    include/gui-qt/game_compatibility.h
    include/gui-qt/game_list_delegate.h
    include/gui-qt/game_list_table.h
    include/gui-qt/game_list.h
    include/gui-qt/game_list_context_menu.h
    include/gui-qt/game_icon_item.h
    include/gui-qt/ctrl_keyboard_filter.h
    include/gui-qt/trophy_notifier.h
    include/gui-qt/trophy_toast.h
    include/gui-qt/trophy_collection_dialog.h
    include/gui-qt/game_overlay_base.h
    include/gui-qt/game_overlay.h
    include/gui-qt/common_dialog_overlay.h
    include/gui-qt/ime_overlay.h
    include/gui-qt/shader_hint_overlay.h
    include/gui-qt/perf_overlay.h
    include/gui-qt/firmware_install_dialog.h
    include/gui-qt/pkg_install_dialog.h
    include/gui-qt/license_install_dialog.h
    include/gui-qt/archive_install_dialog.h
    include/gui-qt/user_management_dialog.h
    include/gui-qt/controls_dialog.h
    include/gui-qt/settings_dialog.h
    include/gui-qt/welcome_dialog.h
    include/gui-qt/qt_utils.h
    include/gui-qt/state.h
    include/gui-qt/gui_save.h
    include/gui-qt/gui_settings_base.h
    include/gui-qt/gui_settings.h
    include/gui-qt/persistent_settings.h

    "../resources.qrc"
)

set_target_properties(gui-qt PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

target_include_directories(gui-qt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(gui-qt PUBLIC
    vita3k::qt6

    config
    ctrl
    dialog
    emuenv
    app
    compat
    io
    np
    kernel
    motion
    packages
    renderer
    touch
    gxm
    audio
    ime

    miniz
    libzRIF
    psvpfsparser
    pugixml::pugixml
    cppcommon
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(gui-qt PRIVATE Qt6::GuiPrivate)
endif()

target_include_directories(gui-qt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
