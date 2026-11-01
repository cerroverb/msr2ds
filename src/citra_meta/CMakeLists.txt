add_executable(citra_meta
    citra.rc
    main.cpp
    precompiled_headers.h
)

set_target_properties(citra_meta PROPERTIES OUTPUT_NAME "azahar")
if (ENABLE_MSR_GAME_FOLDER)
    set_target_properties(citra_meta PROPERTIES OUTPUT_NAME "SamusReturns")
    target_compile_definitions(citra_meta PRIVATE ENABLE_MSR_GAME_FOLDER)
    # The player's own icon and version information instead of the emulator's.
    set_source_files_properties(citra.rc PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(citra_meta PRIVATE msr.rc)
endif()

if (BSD STREQUAL "NetBSD")
    include(DisablePaxMprotect)
    disable_pax_mprotect(citra_meta)
endif()

if (APPLE)
    set(DIST_DIR "../../dist/apple")
    set(APPLE_BUNDLE_NAME "Azahar")
    set(APPLE_BUNDLE_ID "org.azahar-emu.azahar")
    set(APPLE_ICON "azahar.icns")
    if (ENABLE_MSR_GAME_FOLDER)
        # samus.icns: dist/msr/samus.png (tools/msr/make_icon.py) at the macOS icon sizes.
        set(APPLE_BUNDLE_NAME "Samus Returns")
        set(APPLE_BUNDLE_ID "io.github.gminustwo.msr2ds")
        set(APPLE_ICON "samus.icns")
    endif()
    set(APPLE_RESOURCES
        "${DIST_DIR}/${APPLE_ICON}"
        "${DIST_DIR}/launch_logo.png"
    )
    if (IOS)
        list(APPEND APPLE_RESOURCES "${DIST_DIR}/LaunchScreen.storyboard")
    endif()
    target_sources(citra_meta PRIVATE ${APPLE_RESOURCES})

    # Define app bundle metadata.
    include(GenerateBuildInfo)
    set(SRC_DIR "${PROJECT_SOURCE_DIR}")
    generate_build_info()
    set(APPLE_BUNDLE_VERSION "${BUILD_VERSION}")
    set(APPLE_SHORT_VERSION "${BUILD_FULLNAME}")
    set(APPLE_LONG_VERSION "${BUILD_FULLNAME}")
    if (ENABLE_MSR_GAME_FOLDER)
        # The port's version (src/msr/version.h): 1.1.0 for Finder, 1.1.0-pre in full.
        file(STRINGS "${PROJECT_SOURCE_DIR}/src/msr/version.h" MSR_VERSION_LINE
             REGEX "char Version\\[\\] = ")
        string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" APPLE_LONG_VERSION "${MSR_VERSION_LINE}")
        string(REGEX REPLACE "-.*" "" APPLE_SHORT_VERSION "${APPLE_LONG_VERSION}")
        set(APPLE_BUNDLE_VERSION "${APPLE_SHORT_VERSION}")
    endif()
    set_target_properties(citra_meta PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${DIST_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${APPLE_BUNDLE_NAME}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${APPLE_BUNDLE_ID}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APPLE_BUNDLE_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APPLE_SHORT_VERSION}"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${APPLE_LONG_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "${APPLE_ICON}"
        RESOURCE "${APPLE_RESOURCES}"
    )

    if (IOS)
        set_target_properties(citra_meta PROPERTIES
            # Have Xcode copy and sign MoltenVK into app bundle.
            XCODE_EMBED_FRAMEWORKS "${MOLTENVK_LIBRARY}"
            XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY YES
            XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks"
            # Support iPhone and iPad.
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )
    endif()
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND MINGW)
    # TODO: Do this for all executables, not just citra_meta
    # TODO: Don't hardcode MXE directory?
    set(dllwalker_command "${CMAKE_SOURCE_DIR}/externals/dllwalker/dllwalker.rb"
                          $<TARGET_FILE:citra_meta>
                          /opt/mxe/usr/x86_64-w64-mingw32.shared/bin/
                          /opt/mxe/usr/x86_64-w64-mingw32.shared/qt6/bin/
                          /opt/mxe/usr/x86_64-w64-mingw32.shared/x86_64-w64-mingw32/bin/)
    set(dll_list_filename "${CMAKE_CURRENT_BINARY_DIR}/required_dlls_list.txt")
    add_custom_command(TARGET citra_meta POST_BUILD
        COMMAND ${dllwalker_command} > ${dll_list_filename}
    )
    add_custom_command(TARGET citra_meta POST_BUILD
        COMMAND cat ${dll_list_filename} | xargs -I {} ${CMAKE_COMMAND} -E copy {} $<TARGET_FILE_DIR:citra_meta>
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:citra_meta>/plugins/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory /opt/mxe/usr/x86_64-w64-mingw32.shared/qt6/plugins/ "$<TARGET_FILE_DIR:citra_meta>/plugins/"
        COMMAND_EXPAND_LISTS
        DEPENDS ${dll_list_filename}
    )
endif()

target_link_libraries(citra_meta PRIVATE citra_cli citra_common fmt)

if (ENABLE_QT)
    target_link_libraries(citra_meta PRIVATE citra_qt)
    target_link_libraries(citra_meta PRIVATE Boost::boost Qt6::Widgets)
endif()

if (ENABLE_ROOM)
    target_link_libraries(citra_meta PRIVATE citra_room)
endif()

if (ENABLE_QT AND UNIX AND NOT APPLE)
    target_link_libraries(citra_meta PRIVATE Qt6::DBus gamemode)
endif()

if (ENABLE_QT AND APPLE)
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
    target_link_libraries(citra_meta PRIVATE Qt6::GuiPrivate)
endif()

if (ENABLE_QT AND ENABLE_DISCORD_RPC)
    target_link_libraries(citra_meta PRIVATE discord-rpc)
endif()

if(WIN32)
    # compile as a win32 gui application instead of a console application
    if(MSVC)
        set_target_properties(citra_meta PROPERTIES LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS")
    elseif(MINGW)
        set_target_properties(citra_meta PROPERTIES LINK_FLAGS_RELEASE "-mwindows")
    endif()
endif()

if (CITRA_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(citra_meta PRIVATE precompiled_headers.h)
endif()

# Bundle in-place on MSVC so dependencies can be resolved by builds.
if (ENABLE_QT AND MSVC)
    include(BundleTarget)
    qt_bundle_target_in_place(citra_meta)
endif()

if(UNIX AND NOT APPLE)
    install(TARGETS citra_meta RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")
endif()
