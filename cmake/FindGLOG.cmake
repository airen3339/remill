# https://github.com/yahoo/caffe/blob/master/cmake/Modules/FindGlog.cmake

# - Try to find the Google Glog library
#
# This module defines the following variables
#
# GLOG_FOUND - Was Glog found
# GLOG_INCLUDE_DIRS - the Glog include directories
# GLOG_LIBRARIES - Link to this
#
# This module accepts the following variables
#
# GLOG_ROOT - Can be set to Glog install path or Windows build path
#

if (NOT DEFINED GLOG_ROOT)
     set (GLOG_ROOT /usr /usr/local)
endif (NOT DEFINED GLOG_ROOT)

if(MSVC)
     set(LIB_PATHS ${GLOG_ROOT} ${GLOG_ROOT}/Release)
else(MSVC)
     set (LIB_PATHS ${GLOG_ROOT} ${GLOG_ROOT}/lib)
endif(MSVC)

macro(_FIND_GLOG_LIBRARIES _var)
     find_library(${_var}
          NAMES  ${ARGN}
          PATHS ${LIB_PATHS} /opt/local/lib
                             /usr/lib/x86_64-linux-gnu
                             /usr/local/lib
                             /usr/lib
          PATH_SUFFIXES lib
      )
     mark_as_advanced(${_var})
endmacro()

macro(_GLOG_APPEND_LIBRARIES _list _release)
set(_debug ${_release}_DEBUG)
if(${_debug})
     set(${_list} ${${_list}} optimized ${${_release}} debug ${${_debug}})
else()
     set(${_list} ${${_list}} ${${_release}})
endif()
endmacro()


# Linux/OS X builds
find_path(GLOG_INCLUDE_DIR NAMES raw_logging.h
    PATHS ${GLOG_ROOT}/include/glog
          /usr/include/glog
          /opt/local/include/glog   # default location in Macports
)

# Find the libraries
# Linux/OS X builds
if(UNIX)
    _FIND_GLOG_LIBRARIES(GLOG_LIBRARIES libglog.so)
endif(UNIX)
if(APPLE)
    _FIND_GLOG_LIBRARIES(GLOG_LIBRARIES libglog.dylib)
endif(APPLE)

if(GLOG_FOUND)
    message(STATUS "glog library found at ${GLOG_LIBRARIES}")
endif()

# handle the QUIETLY and REQUIRED arguments and set GLOG_FOUND to TRUE if
# all listed variables are TRUE
include("${CMAKE_ROOT}/Modules/FindPackageHandleStandardArgs.cmake")
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Glog DEFAULT_MSG
     GLOG_LIBRARIES)

# Linux/OS X builds
set(GLOG_INCLUDE_DIRS ${GLOG_INCLUDE_DIR})
string(REGEX REPLACE "/libglog.so" "" GLOG_LIBRARIES_DIR ${GLOG_LIBRARIES})

if(GLOG_FOUND)
    message(STATUS "Found glog  (include: ${GLOG_INCLUDE_DIRS}, library: ${GLOG_LIBRARIES})")
      # _GLOG_APPEND_LIBRARIES(GLOG GLOG_LIBRARIES)
endif()