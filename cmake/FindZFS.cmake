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
#
# Find ZFS user library files and set up a set of imported targets for them
#
# inputs:
#   - ZFS_INCLUDE_DIR: hint for finding libzfs.h
#   - ZFS_LIBRARY_DIR: hint for finding ZFS user libraries
#
# output:
#   - a set of ZFS library targets
#   - ZFS_FOUND  (set if found)
#
include(FindPackageHandleStandardArgs)

find_path(ZFS_INCLUDE libzfs.h HINTS ${ZFS_INCLUDE_DIR})

find_library(NVPAIR_LIBRARY nvpair HINTS ${ZFS_LIBRARY_DIR})
find_library(ZPOOL_LIBRARY zpool HINTS ${ZFS_LIBRARY_DIR})
find_library(ZFS_LIBRARY zfs HINTS ${ZFS_LIBRARY_DIR})

find_package_handle_standard_args(ZFS DEFAULT_MSG ZFS_INCLUDE
        NVPAIR_LIBRARY ZPOOL_LIBRARY ZFS_LIBRARY)
mark_as_advanced(ZFS_INCLUDE NVPAIR_LIBRARY ZPOOL_LIBRARY ZFS_LIBRARY)

if (ZFS_FOUND)
    if (NOT TARGET nvpair)
        add_library(nvpair UNKNOWN IMPORTED)
        set_target_properties(nvpair PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${ZFS_INCLUDE}")
        set_property(TARGET nvpair APPEND PROPERTY
                IMPORTED_LOCATION "${NVPAIR_LIBRARY}")
    endif ()
    if (NOT TARGET zpool)
        add_library(zpool UNKNOWN IMPORTED)
        set_target_properties(zpool PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${ZFS_INCLUDE}")
        set_property(TARGET zpool APPEND PROPERTY
                IMPORTED_LOCATION "${ZPOOL_LIBRARY}")
    endif ()
    if (NOT TARGET zfs)
        add_library(zfs UNKNOWN IMPORTED)
        set_target_properties(zfs PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${ZFS_INCLUDE}")
        set_property(TARGET zfs APPEND PROPERTY
                IMPORTED_LOCATION "${ZFS_LIBRARY}")
    endif ()
endif ()
