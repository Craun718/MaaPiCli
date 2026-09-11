cmake_minimum_required(VERSION 3.28)

project(MaaPiCli VERSION 0.1.0 LANGUAGES CXX)

option(MaaPiCli_BUILD_EXECUTABLE "build the MaaPiCli executable" ON)
option(MaaPiCli_BUILD_TESTS "build the MaaPiCli tests" OFF)

find_package(Boost REQUIRED CONFIG COMPONENTS system regex)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(MaaFramework CONFIG REQUIRED)

file(GLOB_RECURSE maa_pi_cli_header include/ProjectInterface/*.h)

set(maa_pi_cli_core_src
    CLI/interactor.cpp
    CLI/interactor.h
    Impl/Configurator.cpp
    Impl/Parser.cpp
    Impl/Runner.cpp)

add_library(MaaPiCliCore STATIC ${maa_pi_cli_core_src} ${maa_pi_cli_header})
add_library(MaaPiCli::Core ALIAS MaaPiCliCore)
set_target_properties(MaaPiCliCore PROPERTIES EXPORT_NAME Core)

target_include_directories(
    MaaPiCliCore
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>)

target_compile_features(MaaPiCliCore PUBLIC cxx_std_20)
target_link_libraries(
    MaaPiCliCore
    PUBLIC MaaFramework::MaaFramework
           MaaFramework::MaaToolkit
           MaaFramework::MaaAgentClient
           MaaFramework::MaaUtils
           Boost::system
           Boost::regex
           ${OpenCV_LIBS})

if(LINUX)
    find_package(Threads REQUIRED)
    target_link_libraries(MaaPiCliCore PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${maa_pi_cli_core_src})

install(
    TARGETS MaaPiCliCore
    EXPORT MaaPiCliTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION bin
    INCLUDES DESTINATION include)

install(DIRECTORY include/ProjectInterface DESTINATION include)

install(
    EXPORT MaaPiCliTargets
    FILE MaaPiCliTargets.cmake
    NAMESPACE MaaPiCli::
    DESTINATION share/cmake/MaaPiCli)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    cmake/MaaPiCliConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/MaaPiCliConfig.cmake"
    INSTALL_DESTINATION share/cmake/MaaPiCli)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MaaPiCliConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MaaPiCliConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/MaaPiCliConfigVersion.cmake"
        DESTINATION share/cmake/MaaPiCli)

if(MaaPiCli_BUILD_EXECUTABLE)
    add_executable(MaaPiCli CLI/main.cpp)

    target_link_libraries(MaaPiCli PRIVATE MaaPiCli::Core)

    if(APPLE)
        set_target_properties(MaaPiCli PROPERTIES INSTALL_RPATH "@executable_path")
    elseif(UNIX)
        set_target_properties(MaaPiCli PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()

    install(TARGETS MaaPiCli RUNTIME DESTINATION bin)

    if(WIN32)
        install(FILES $<TARGET_PDB_FILE:MaaPiCli> DESTINATION symbol OPTIONAL)
    endif()
endif()

if(MaaPiCli_BUILD_TESTS)
    enable_testing()

    add_executable(MaaPiCliParserTest test/parser/parser_test.cpp)
    target_compile_features(MaaPiCliParserTest PRIVATE cxx_std_20)
    target_link_libraries(MaaPiCliParserTest PRIVATE MaaPiCli::Core)
    target_compile_definitions(
        MaaPiCliParserTest PRIVATE MAAPICLI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/parser/fixtures")

    add_test(NAME MaaPiCliParserTest COMMAND MaaPiCliParserTest)
    set_tests_properties(
        MaaPiCliParserTest
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:MaaFramework::MaaUtils>")

    add_executable(MaaPiCliPretaskTest test/pretask/pretask_test.cpp)
    target_compile_features(MaaPiCliPretaskTest PRIVATE cxx_std_20)
    target_link_libraries(MaaPiCliPretaskTest PRIVATE MaaPiCli::Core)
    target_compile_definitions(
        MaaPiCliPretaskTest
        PRIVATE MAAPICLI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/parser/fixtures")

    add_test(NAME MaaPiCliPretaskTest COMMAND MaaPiCliPretaskTest)
    set_tests_properties(
        MaaPiCliPretaskTest
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:MaaFramework::MaaUtils>")
endif()
