# Remmina - The GTK+ Remote Desktop Client
#
# Copyright (C) 2026 Remmina contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.

# - Find sso-mib
# Find the sso-mib identity broker library (https://github.com/siemens/sso-mib)
#
# SSO_MIB_FOUND - system has sso-mib
# SSO_MIB_INCLUDE_DIRS - the sso-mib include directories
# SSO_MIB_LIBRARIES - link these to use sso-mib

include(FindPackageHandleStandardArgs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_SSO_MIB sso-mib>=0.5.0)

if(PC_SSO_MIB_FOUND)
  find_path(SSO_MIB_INCLUDE_DIR NAMES sso-mib/sso-mib.h HINTS ${PC_SSO_MIB_INCLUDEDIR})
  find_library(SSO_MIB_LIBRARY NAMES sso-mib HINTS ${PC_SSO_MIB_LIBRARYDIR})

  find_package_handle_standard_args(SSO_MIB DEFAULT_MSG SSO_MIB_LIBRARY SSO_MIB_INCLUDE_DIR)

  if(SSO_MIB_FOUND)
    set(SSO_MIB_LIBRARIES ${SSO_MIB_LIBRARY} ${PC_SSO_MIB_LIBRARIES})
    set(SSO_MIB_INCLUDE_DIRS ${SSO_MIB_INCLUDE_DIR} ${PC_SSO_MIB_INCLUDE_DIRS})
  endif()
endif()

mark_as_advanced(SSO_MIB_INCLUDE_DIR SSO_MIB_LIBRARY)
