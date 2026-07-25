# print-pin.cmake — print one pin from dependencies.cmake on stdout, so shell
# consumers read pins through CMake instead of regexing the file:
#
#   cmake -DPIN=MOXYGEN_REV -P cmake/print-pin.cmake
#
# message() writes to stderr in script mode; -E echo is the stdout channel.
include("${CMAKE_CURRENT_LIST_DIR}/dependencies.cmake")
if(NOT DEFINED PIN)
  message(FATAL_ERROR "usage: cmake -DPIN=<name> -P cmake/print-pin.cmake")
endif()
if(NOT DEFINED ${PIN})
  message(FATAL_ERROR "print-pin: '${PIN}' is not set in cmake/dependencies.cmake")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${${PIN}}")
