# Copyright (c) 2021 Triad National Security, LLC, as operator of Los Alamos
# National Laboratory with the U.S. Department of Energy/National Nuclear
# Security Administration. All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# with the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
# 3. Neither the name of TRIAD, Los Alamos National Laboratory, LANL, the
#    U.S. Government, nor the names of its contributors may be used to endorse
#    or promote products derived from this software without specific prior
#    written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
# EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
cmake_minimum_required(VERSION 2.8.12)
project (c2_libzdb C CXX)

enable_testing ()
set(CMAKE_MODULE_PATH
        ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_PREFIX_PATH "" CACHE STRING "External dependencies path")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()
set(BUILD_SHARED_LIBS "OFF" CACHE BOOL "Build a shared library")
set(BUILD_TESTS "OFF" CACHE BOOL "Build test programs")
# CMAKE_CXX_STANDARD is not defined until cmake 3.1, and will be ignored by
# cmake 3.0 and before.
set(C2_CXX_STANDARD "11" CACHE STRING "C++ std to probe")
set(C2_CXX_STANDARD_REQUIRED "OFF" CACHE BOOL "C++ std must be met")
mark_as_advanced(C2_CXX_STANDARD C2_CXX_STANDARD_REQUIRED)
set_property(CACHE C2_CXX_STANDARD PROPERTY STRINGS "98" "11")
include(CheckCXXCompilerFlag)
if(CMAKE_VERSION VERSION_LESS "3.1")
    set(cxxstdflag "-std=c++${C2_CXX_STANDARD}")
    CHECK_CXX_COMPILER_FLAG(${cxxstdflag} flag${cxxstdflag})
    if(${flag${cxxstdflag}})
        add_compile_options(${cxxstdflag})
    else()
        if(C2_CXX_STANDARD_REQUIRED)
            message(FATAL_ERROR "Fail to enable CXX ${C2_CXX_STANDARD}")
        endif()
    endif()
else()
    set(CMAKE_CXX_STANDARD ${C2_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD_REQUIRED ${C2_CXX_STANDARD_REQUIRED})
endif()

set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
# link shared lib with full rpath. on some modern platforms, the newer
# runpath will be used instead of rpath.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
# quiet CMP0042 warning  (not needed if cmake >=3.0)
set(CMAKE_MACOSX_RPATH 1)

find_package(SPL MODULE REQUIRED)
find_package(ZFS MODULE REQUIRED)

add_subdirectory(src)
