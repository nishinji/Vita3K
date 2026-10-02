cmake_minimum_required(VERSION 3.22)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
	set(ANDROID 1)
	set(VCPKG_TARGET_ANDROID 1)
	# this file must be included before everything else
	include("cmake/vcpkg_android.cmake")
endif()

project(Vita3K)

# Detects the amount of processors of the host machine and forwards the result to CPU_COUNT
include(ProcessorCount)
ProcessorCount(CPU_COUNT)

# Define the Architecture variable, right now it should only contain "x86_64" or "arm64"
include("external/dynarmic/CMakeModules/DetectArchitecture.cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3)

# workaround for SDL due to cmake bug
# https://github.com/libsdl-org/SDL/issues/6454
# https://gitlab.kitware.com/cmake/cmake/-/issues/24104
if(APPLE)
    enable_language(OBJC)
endif()

if(ANDROID)
	set(CMAKE_SYSROOT "")
	set(CMAKE_OSX_SYSROOT "")
	set_property(GLOBAL
	PROPERTY
	NO_SYSTEM_FROM_IMPORTED ON
	)

	string(APPEND CMAKE_CXX_FLAGS " -Wl,-Bsymbolic -Wno-unused-command-line-argument")
	string(APPEND CMAKE_C_FLAGS " -Wl,-Bsymbolic -Wno-unused-command-line-argument")

	if(CMAKE_BUILD_TYPE STREQUAL "Release")
		set(CMAKE_C_VISIBILITY_PRESET hidden)
		set(CMAKE_CXX_VISIBILITY_PRESET hidden)
		set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)
	endif()
else()
	if((ARCHITECTURE STREQUAL "x86_64") OR APPLE)
		option(USE_DISCORD_RICH_PRESENCE "Build Vita3K with Discord Rich Presence" ON)
	endif()
	option(BUILD_APPIMAGE "Build an AppImage." OFF)
endif()

option(FORCE_BUILD_OPENSSL_MAC OFF)

if("${CMAKE_CXX_COMPILER_LAUNCHER}" STREQUAL "")
	find_program(CCACHE_PROGRAM ccache)
	if(CCACHE_PROGRAM)
		set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
		set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
	endif()
endif()

if(MSVC)
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
	string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
	string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
endif()

enable_testing()

cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(lto_usage_mode ALWAYS RELEASE_ONLY NEVER)
set(USE_LTO RELEASE_ONLY CACHE STRING "Use interprocedural optimization/link time optimization")
set_property(CACHE USE_LTO PROPERTY STRINGS ${lto_usage_mode})

if (NOT (USE_LTO STREQUAL "NEVER"))
	include(CheckIPOSupported)
	if(ANDROID)
		# https://gitlab.kitware.com/cmake/cmake/-/issues/21772
		STRING(REPLACE "-fuse-ld=gold" "" CMAKE_CXX_LINK_OPTIONS_IPO "${CMAKE_CXX_LINK_OPTIONS_IPO}")
		STRING(REPLACE "-fuse-ld=gold" "" CMAKE_C_LINK_OPTIONS_IPO "${CMAKE_C_LINK_OPTIONS_IPO}")
		set(ipo_supported TRUE)
	else()
		check_ipo_supported(RESULT ipo_supported OUTPUT ipo_supported_error)
	endif()

	if( ipo_supported )
		if (USE_LTO STREQUAL "ALWAYS")
			set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
		elseif (USE_LTO STREQUAL "RELEASE_ONLY")
			set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
		endif()
	else()
		message(STATUS "IPO / LTO not supported: <${ipo_supported_error}>")
		set(USE_LTO NEVER CACHE STRING "Use interprocedural optimization/link time optimization")
	endif()
endif()


############################
########## Boost ###########
############################

# Boost modules that have to be linked against (i.e. the ones that aren't header only)
set(BOOST_MODULES_TO_FIND filesystem)

option(VITA3K_FORCE_CUSTOM_BOOST "Force Vita3K build process to use the Boost version included with the repository" OFF)
option(VITA3K_FORCE_SYSTEM_BOOST "Force Vita3K build process to use the Boost version in the system and CMake's default paths and ignore the Boost version included with Vita3K" OFF)

# Look for a Boost distribution, falling back to the one bundled with the repository
macro(get_boost)
	# find_package(Boost ...) settings
	set(Boost_USE_STATIC_LIBS ON)

	if(VITA3K_FORCE_SYSTEM_BOOST)
		# Only look for Boost on the system and CMake's default paths
		find_package(Boost 1.81 COMPONENTS ${BOOST_MODULES_TO_FIND} REQUIRED)
	else()
		set(Boost_NO_WARN_NEW_VERSIONS 1)

		# First, try to find Boost without any hints (system Boost)
		if(NOT VITA3K_FORCE_CUSTOM_BOOST)
			find_package(Boost 1.81 COMPONENTS ${BOOST_MODULES_TO_FIND} QUIET)
		endif()

		# Otherwise, fall back to the distribution bundled in external/boost. It
		# ships neither a CMake package nor a usable copy of the b2 buildsystem,
		# so Boost.Filesystem is compiled as a regular CMake target (named
		# boost_filesystem) by external/CMakeLists.txt instead.
		if(NOT Boost_FOUND)
			message(STATUS "A Boost distribution that meets the requirements needed for the project hasn't been found in your system. Falling back to custom Boost distribution...")

			set(BOOST_SOURCEDIR "${CMAKE_SOURCE_DIR}/external/boost")

			if(NOT EXISTS "${BOOST_SOURCEDIR}/boost/version.hpp")
				message(FATAL_ERROR "Submodule external/boost is empty. "
					"Please run: git submodule update --init --recursive")
			endif()

			set(VITA3K_USE_CUSTOM_BOOST ON)

			# Let the external projects that look for Boost on their own pick up
			# the bundled headers, as they only use the header only part of Boost
			set(Boost_INCLUDE_DIR "${BOOST_SOURCEDIR}" CACHE PATH "Path to the Boost headers" FORCE)

			set(Boost_INCLUDE_DIRS "${BOOST_SOURCEDIR}")
			set(Boost_LIBRARIES boost_filesystem)
			set(Boost_FOUND TRUE)

			message(STATUS "Custom Boost distribution found in ${BOOST_SOURCEDIR}")
		endif()
	endif()
endmacro(get_boost)

get_boost()

# The bundled Boost distribution isn't built with b2, so its libraries don't
# follow the naming scheme expected by the autolinking of MSVC like toolchains
if(VITA3K_USE_CUSTOM_BOOST)
	add_compile_definitions("BOOST_ALL_NO_LIB")
endif()

if(WIN32)
	add_compile_definitions("_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS" "_CRT_SECURE_NO_WARNINGS" "NOMINMAX")
endif()

# Disable inclusion of timestamps in precompiled headers on Windows
if(CMAKE_HOST_WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
	string(APPEND CMAKE_C_FLAGS " -Xclang -fno-pch-timestamp")
	string(APPEND CMAKE_CXX_FLAGS " -Xclang -fno-pch-timestamp")
endif()

# Allow per-translation-unit parallel builds when using MSVC
if(CMAKE_GENERATOR MATCHES "Visual Studio" AND (CMAKE_C_COMPILER_ID MATCHES "MSVC|Intel|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "MSVC|Intel|Clang"))
	string(APPEND CMAKE_C_FLAGS " /MP")
	string(APPEND CMAKE_CXX_FLAGS " /MP")
endif()

if (${BUILD_APPIMAGE})
	if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(LINUXDEPLOY_COMMAND "${CMAKE_SOURCE_DIR}/appimage/linuxdeploy.AppImage" CACHE INTERNAL "")
		if (NOT EXISTS "${LINUXDEPLOY_COMMAND}")
			message(FATAL_ERROR "Could not find linuxdeploy at ${LINUXDEPLOY_COMMAND}!")
		endif()
	else()
		message(FATAL_ERROR "Cannot build an AppImage for a non-Linux host.")
	endif()
endif()

add_subdirectory(external)
add_subdirectory(vita3k)
add_subdirectory(tools/gen-modules)
