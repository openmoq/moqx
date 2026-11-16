# Partial-link moq-peering and its static picoquic into one object, then make
# every symbol except MoqPeeringRuntime local. The relay already links a
# different picoquic; the two copies share a process only if their symbols
# do not collide.
#
# Arguments after -- are the runtime object, then the static archives.
# -DMOQX_COMPILER, -DMOQX_NM, -DMOQX_OUTPUT, -DMOQX_APPLE, -DMOQX_OBJCOPY.
cmake_minimum_required(VERSION 3.23)

set(_past FALSE)
set(_runtime "")
set(_archives "")
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 0 ${_last})
  set(_arg "${CMAKE_ARGV${_i}}")
  if(NOT _past)
    if(_arg STREQUAL "--")
      set(_past TRUE)
    endif()
    continue()
  endif()
  if(_runtime STREQUAL "")
    set(_runtime "${_arg}")
  else()
    list(APPEND _archives "${_arg}")
  endif()
endforeach()

if(_runtime STREQUAL "" OR NOT _archives)
  message(FATAL_ERROR "bundle-peering: expected a runtime object and static archives")
endif()
if(NOT MOQX_COMPILER OR NOT MOQX_OUTPUT)
  message(FATAL_ERROR "bundle-peering: MOQX_COMPILER and MOQX_OUTPUT are required")
endif()

# -fno-lto keeps this relocatable object as native code when the rest of the
# link is -flto. Hidden symbols were localized in that native symbol table.
set(_cmd "${MOQX_COMPILER}" -r -nostdlib -fno-lto -o "${MOQX_OUTPUT}" "${_runtime}")
list(APPEND _cmd ${_archives})

if(MOQX_APPLE)
  if(NOT MOQX_NM)
    message(FATAL_ERROR "bundle-peering: MOQX_NM is required")
  endif()
  execute_process(
    COMMAND "${MOQX_NM}" -m "${_runtime}"
    OUTPUT_VARIABLE _nm
    COMMAND_ERROR_IS_FATAL ANY
  )
  string(REPLACE "\n" ";" _lines "${_nm}")
  set(_keep "")
  foreach(_line IN LISTS _lines)
    if(_line MATCHES " undefined " OR _line MATCHES " private external " OR _line MATCHES " weak ")
      continue()
    endif()
    if(_line MATCHES " external ")
      string(REGEX MATCH "[^ ]+$" _sym "${_line}")
      list(APPEND _keep "${_sym}")
    endif()
  endforeach()
  if(NOT _keep)
    message(FATAL_ERROR "bundle-peering: no exported symbols in ${_runtime}")
  endif()
  set(_keep_file "${MOQX_OUTPUT}.exports")
  list(JOIN _keep "\n" _keep_text)
  file(WRITE "${_keep_file}" "${_keep_text}\n")
  list(APPEND _cmd "-Wl,-exported_symbols_list,${_keep_file}")
endif()

execute_process(COMMAND ${_cmd} COMMAND_ERROR_IS_FATAL ANY)

if(NOT MOQX_APPLE)
  if(NOT MOQX_OBJCOPY)
    message(FATAL_ERROR "bundle-peering: objcopy is required to localize hidden symbols")
  endif()
  execute_process(
    COMMAND "${MOQX_OBJCOPY}" --localize-hidden "${MOQX_OUTPUT}"
    COMMAND_ERROR_IS_FATAL ANY
  )
endif()
