cmake_minimum_required(VERSION 3.10)
project(mainui LANGUAGES CXX)

option(BUILD_AS_PART_OF_ENGINE "Build as part of Xash3D engine" OFF)

file(GLOB MAINUI_ROOT_SOURCES "*.cpp")
file(GLOB MAINUI_MINIUTL_SOURCES "miniutl/*.cpp")
file(GLOB MAINUI_FONT_SOURCES "font/*.cpp")
file(GLOB MAINUI_MENUS_SOURCES "menus/*.cpp")
file(GLOB MAINUI_MENUS_DYNAMIC_SOURCES "menus/dynamic/*.cpp")
file(GLOB MAINUI_MODEL_SOURCES "model/*.cpp")
file(GLOB MAINUI_CONTROLS_SOURCES "controls/*.cpp")

set(MAINUI_SOURCES
	${MAINUI_ROOT_SOURCES}
	${MAINUI_MINIUTL_SOURCES}
	${MAINUI_FONT_SOURCES}
	${MAINUI_MENUS_SOURCES}
	${MAINUI_MENUS_DYNAMIC_SOURCES}
	${MAINUI_MODEL_SOURCES}
	${MAINUI_CONTROLS_SOURCES}
)

add_library(menu SHARED ${MAINUI_SOURCES})

target_include_directories(menu PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/miniutl
	${CMAKE_CURRENT_SOURCE_DIR}/font
	${CMAKE_CURRENT_SOURCE_DIR}/controls
	${CMAKE_CURRENT_SOURCE_DIR}/menus
	${CMAKE_CURRENT_SOURCE_DIR}/model
	${CMAKE_CURRENT_SOURCE_DIR}/sdk_includes/common
	${CMAKE_CURRENT_SOURCE_DIR}/sdk_includes/engine
	${CMAKE_CURRENT_SOURCE_DIR}/sdk_includes/public
	${CMAKE_CURRENT_SOURCE_DIR}/sdk_includes/pm_shared
)

target_compile_definitions(menu PRIVATE
	STDINT_H=<cstdint>
	MAINUI_USE_CUSTOM_FONT_RENDER=1
)

if(ANDROID)
	target_compile_definitions(menu PRIVATE
		MAINUI_USE_STB=1
		NO_STL=1
	)
	target_compile_options(menu PRIVATE
		-fno-rtti
		-fno-exceptions
	)
else()
	target_compile_options(menu PRIVATE
		-fno-rtti
	)
endif()

install(TARGETS menu
	LIBRARY DESTINATION lib
	RUNTIME DESTINATION lib
)
