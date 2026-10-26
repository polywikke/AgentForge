cmake_minimum_required(VERSION 3.15)
project(AgentForge VERSION 2.0 LANGUAGES CXX C)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含目录
include_directories(${PROJECT_SOURCE_DIR}/include)

# nlohmann/json 库 (header-only, 位于 include/nlohmann/json.hpp)
find_package(nlohmann_json 3.11.0 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json package not found, using bundled header: include/nlohmann/json.hpp")
endif()

# libcurl 可选 (仅 curl_transport 使用, 默认不参与构建)
find_package(CURL QUIET)

# tree-sitter (L8): 真实语法分析 + 词法兜底
add_library(agentforge_treesitter STATIC
    thirdparty/tree-sitter/core/src/lib.c
    thirdparty/tree-sitter/cpp/src/parser.c
    thirdparty/tree-sitter/cpp/src/scanner.c
)
target_include_directories(agentforge_treesitter PUBLIC
    thirdparty/tree-sitter/core/include
    thirdparty/tree-sitter/core/src
)
if(MSVC)
    target_compile_options(agentforge_treesitter PRIVATE /bigobj /O2 /utf-8)
endif()

# 共享源文件
set(CORE_SOURCES
    src/core_tools.cpp
    src/ui_impl.cpp
    src/mock_provider.cpp
    src/sandbox_win.cpp
    src/symbol_extract.cpp
)

# V1 可执行文件 (L1-L5 简化版, Mock Provider 演示)
add_executable(agentforge
    src/main.cpp
    ${CORE_SOURCES}
)

# V2 可执行文件 (L1-L12 完整版)
add_executable(agentforge_v2
    src/main_v2.cpp
    ${CORE_SOURCES}
)

# Web 服务器可执行文件 (HTTP API + Web UI)
add_executable(agentforge_web
    src/main_web_simple.cpp
    src/core_tools.cpp
    src/sandbox_win.cpp
    src/simple_http_transport.cpp
    src/multi_provider.cpp
    src/symbol_extract.cpp
)

# 链接库
if(nlohmann_json_FOUND)
    target_link_libraries(agentforge PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(agentforge_v2 PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(agentforge_web PRIVATE nlohmann_json::nlohmann_json)
endif()

# Windows 特定设置
if(WIN32)
    foreach(tgt agentforge agentforge_v2 agentforge_web)
        target_compile_definitions(${tgt} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            AF_HAVE_TREESITTER
        )
        target_link_libraries(${tgt} PRIVATE agentforge_treesitter)
    endforeach()

    target_compile_definitions(agentforge_web PRIVATE USE_HTTPLIB USE_WINHTTP)

    # Web 服务器需要 Winsock2 和 WinHTTP
    target_link_libraries(agentforge_web PRIVATE ws2_32 winhttp)
    target_link_libraries(agentforge PRIVATE winhttp)
    target_link_libraries(agentforge_v2 PRIVATE winhttp)
else()
    message(WARNING "Web server currently only supports Windows")
endif()

# 编译选项
if(MSVC)
    foreach(tgt agentforge agentforge_v2 agentforge_web)
        target_compile_options(${tgt} PRIVATE /W3 /utf-8)
    endforeach()
else()
    foreach(tgt agentforge agentforge_v2 agentforge_web)
        target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    endforeach()
endif()

# 设置输出目录 (多配置生成器会追加 Debug/Release 子目录)
set_target_properties(agentforge agentforge_v2 agentforge_web PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin
)

# 安装规则
install(TARGETS agentforge agentforge_v2 agentforge_web DESTINATION bin)

# 打印配置信息
message(STATUS "===========================================")
message(STATUS "  AgentForge Configuration")
message(STATUS "===========================================")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "")
message(STATUS "  Targets:")
message(STATUS "    - agentforge     (V1: L1-L5)")
message(STATUS "    - agentforge_v2  (V2: L1-L12)")
message(STATUS "    - agentforge_web (Web Server with HTTP API)")
message(STATUS "===========================================")
