cmake_minimum_required(VERSION 3.16)

project(q VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui Network WebEngineWidgets WebChannel)

# Find R
find_program(R_EXECUTABLE R)
if(NOT R_EXECUTABLE)
    message(FATAL_ERROR "R not found. Please install R.")
endif()

execute_process(
    COMMAND ${R_EXECUTABLE} CMD config --ldflags
    OUTPUT_VARIABLE R_LDFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND ${R_EXECUTABLE} CMD config --cppflags
    OUTPUT_VARIABLE R_CPPFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Source files
set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/codeeditor.cpp
    src/codeeditor.h
    src/syntaxhighlighter.cpp
    src/syntaxhighlighter.h
    src/filebrowser.cpp
    src/filebrowser.h
    src/thememanager.cpp
    src/thememanager.h
    src/terminalwidget.h
    src/terminalwidget.cpp   # xterm.js-based, unified for all platforms
    src/xterm.qrc            # bundles xterm.js, xterm.css, terminal.html
    src/environmentpane.cpp
    src/environmentpane.h
    src/helppane.cpp
    src/helppane.h
)


# Create executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Link Qt libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    Qt6::Network
    Qt6::WebEngineWidgets
    Qt6::WebChannel
)

# Link platform PTY library (POSIX only)
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE util)  # for forkpty on Linux
endif()

# Set output directory
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Install target
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Copy gogh-themes into the binary output directory so the app always
# finds `./gogh-themes` next to the executable during development.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/gogh-themes
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/gogh-themes
    COMMENT "Copy gogh-themes to output directory next to the binary"
)

# Install gogh-themes next to the installed binary (so runtime still finds
# `bin/gogh-themes` after `make install`). This keeps the runtime behavior
# consistent between development and installed layouts.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/gogh-themes/
    DESTINATION bin/gogh-themes
    FILES_MATCHING PATTERN "*.json"
)

# Copy fonts into the binary output directory so the app always finds
# `./fonts` next to the executable (useful on systems that don't ship
# the Hack font, etc.).
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/fonts
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/fonts
    COMMENT "Copy fonts to output directory next to the binary"
)

# Install fonts next to the installed binary as well, keeping runtime
# layout consistent between development and installed packages.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/fonts/
    DESTINATION bin/fonts
    FILES_MATCHING PATTERN "*.ttf"
                  PATTERN "*.otf"
)
