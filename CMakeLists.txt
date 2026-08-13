cmake_minimum_required(VERSION 3.16)

project(VideoAnalyzer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Qt 6.8.3 (MinGW 64-bit) ──────────────────────────────────────────
set(CMAKE_PREFIX_PATH "E:/Qt/6.8.3/mingw_64")

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Network)

qt_standard_project_setup()

# ── OpenCV 4.5.5 (MinGW) ─────────────────────────────────────────────
set(OpenCV_DIR "E:/share/opencv")
set(OpenCV_RUNTIME "mingw")
set(OpenCV_ARCH "x64")
find_package(OpenCV REQUIRED)

# ── nlohmann/json (header-only, single include) ──────────────────────
set(NLOHMANN_JSON_INCLUDE_DIR "E:/json-develop/single_include")

# ── 目录 ─────────────────────────────────────────────────────────────
set(VIDEOANALYZER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(VIDEOANALYZER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ── 头文件列表 ───────────────────────────────────────────────────────
set(VIDEOANALYZER_HEADERS
    ${VIDEOANALYZER_INCLUDE_DIR}/VideoAnalyzerController.h
    ${VIDEOANALYZER_INCLUDE_DIR}/StreamManager.h
    ${VIDEOANALYZER_INCLUDE_DIR}/EventLoop.h
    ${VIDEOANALYZER_INCLUDE_DIR}/Channel.h
    ${VIDEOANALYZER_INCLUDE_DIR}/ThreadPool.h
    ${VIDEOANALYZER_INCLUDE_DIR}/EventDetector.h
    ${VIDEOANALYZER_INCLUDE_DIR}/DatabaseManager.h
    ${VIDEOANALYZER_INCLUDE_DIR}/LLMClient.h
    ${VIDEOANALYZER_INCLUDE_DIR}/FeatureExtractor.h
)

# ── 源文件列表 ───────────────────────────────────────────────────────
set(VIDEOANALYZER_SOURCES
    ${VIDEOANALYZER_SRC_DIR}/VideoAnalyzerController.cpp
    ${VIDEOANALYZER_SRC_DIR}/StreamManager.cpp
    ${VIDEOANALYZER_SRC_DIR}/EventLoop.cpp
    ${VIDEOANALYZER_SRC_DIR}/Channel.cpp
    ${VIDEOANALYZER_SRC_DIR}/ThreadPool.cpp
    ${VIDEOANALYZER_SRC_DIR}/EventDetector.cpp
    ${VIDEOANALYZER_SRC_DIR}/FeatureExtractor.cpp
    ${VIDEOANALYZER_SRC_DIR}/DatabaseManager.cpp
    ${VIDEOANALYZER_SRC_DIR}/LLMClient.cpp
)

# ── 目标定义 ─────────────────────────────────────────────────────────
qt_add_executable(VideoAnalyzer
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    ${VIDEOANALYZER_HEADERS}
    ${VIDEOANALYZER_SOURCES}
)

target_include_directories(VideoAnalyzer PRIVATE
    ${VIDEOANALYZER_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)

target_link_libraries(VideoAnalyzer PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::Network
    opencv_world
)

# 运行时 DLL 拷贝（Qt + OpenCV + MinGW 运行时到构建目录，便于直接运行）
add_custom_command(TARGET VideoAnalyzer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Gui>" "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Widgets>" "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Sql>" "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/share/opencv/x64/mingw/bin/libopencv_world455.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/Qt/Tools/mingw1310_64/bin/libgcc_s_seh-1.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/Qt/Tools/mingw1310_64/bin/libwinpthread-1.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/Qt/Tools/mingw1310_64/bin/libstdc++-6.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:VideoAnalyzer>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/Qt/6.8.3/mingw_64/plugins/platforms/qwindows.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>/platforms"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:VideoAnalyzer>/sqldrivers"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/Qt/6.8.3/mingw_64/plugins/sqldrivers/qsqlite.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>/sqldrivers"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "D:/VMware/VMware Workstation/x64/libssl-3-x64.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "D:/VMware/VMware Workstation/x64/libcrypto-3-x64.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:VideoAnalyzer>/tls"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "E:/Qt/6.8.3/mingw_64/plugins/tls/qopensslbackend.dll"
        "$<TARGET_FILE_DIR:VideoAnalyzer>/tls"
    COMMENT "Copying runtime DLLs"
)
