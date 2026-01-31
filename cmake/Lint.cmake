# Lint/format targets
find_program(CLANG_FORMAT clang-format)
find_program(CLANG_TIDY clang-tidy)

file(GLOB_RECURSE ORLY_LINT_SOURCES
  CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/include/*.h
  ${PROJECT_SOURCE_DIR}/include/*.hpp
  ${PROJECT_SOURCE_DIR}/src/*.cc
  ${PROJECT_SOURCE_DIR}/src/*.cpp
  ${PROJECT_SOURCE_DIR}/src/*.cxx
  ${PROJECT_SOURCE_DIR}/tests/*.cc
  ${PROJECT_SOURCE_DIR}/tests/*.cpp
  ${PROJECT_SOURCE_DIR}/tests/*.cxx
  ${PROJECT_SOURCE_DIR}/tools/*.cc
  ${PROJECT_SOURCE_DIR}/tools/*.cpp
  ${PROJECT_SOURCE_DIR}/tools/*.cxx
)

if(CLANG_FORMAT)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i ${ORLY_LINT_SOURCES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Running clang-format"
  )
endif()

if(CLANG_TIDY)
  add_custom_target(lint
    COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${ORLY_LINT_SOURCES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Running clang-tidy"
  )
endif()
