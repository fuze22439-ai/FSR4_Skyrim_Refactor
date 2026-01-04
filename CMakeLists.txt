cmake_minimum_required(VERSION 3.20)

project(
	FSR4_Skyrim
	VERSION 1.0.0
	LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(XSEPlugin)

find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	${CLIB_UTIL_INCLUDE_DIRS}
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	debug ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Debug/detours.lib
	optimized ${CMAKE_CURRENT_SOURCE_DIR}/include/detours/Release/detours.lib
	d3d12.lib
)

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE 
        "/DELAYLOAD:amd_fidelityfx_loader_dx12.dll"
        "delayimp.lib"
    )
endif()

include(FidelityFX-SDK)