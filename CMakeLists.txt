cmake_minimum_required(VERSION 3.16)

project(favo VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(FAVO_BUILD_TESTS "Compila os testes" ON)
option(FAVO_BUILD_TOOLS "Compila a linha de comando" ON)
option(FAVO_SANITIZE "Ativa AddressSanitizer e UndefinedBehaviorSanitizer" OFF)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)
  if(FAVO_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

set(FAVO_SOURCES
  src/os.c
  src/list.c
  src/rbtree.c
  src/heap.c
  src/inspect.c
  src/global.c
  src/bench.c
)

find_package(Threads REQUIRED)

add_library(favo_static STATIC ${FAVO_SOURCES})
target_include_directories(favo_static PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_definitions(favo_static PRIVATE FAVO_BUILDING)
target_link_libraries(favo_static PUBLIC Threads::Threads)
set_target_properties(favo_static PROPERTIES OUTPUT_NAME favo_static)

add_library(favo SHARED ${FAVO_SOURCES})
target_include_directories(favo PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_definitions(favo PRIVATE FAVO_BUILDING PUBLIC FAVO_SHARED)
target_link_libraries(favo PUBLIC Threads::Threads)
set_target_properties(favo PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR} C_VISIBILITY_PRESET hidden)

if(UNIX AND NOT APPLE)
  add_library(favo_preload SHARED ${FAVO_SOURCES} src/preload.c)
  target_include_directories(favo_preload PRIVATE include)
  target_compile_definitions(favo_preload PRIVATE FAVO_BUILDING)
  target_link_libraries(favo_preload PRIVATE Threads::Threads)
  set_target_properties(favo_preload PROPERTIES C_VISIBILITY_PRESET hidden)
  if(FAVO_SANITIZE)
    set_target_properties(favo_preload PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()
endif()

if(FAVO_BUILD_TOOLS)
  add_executable(favo_cli tools/favo.c tools/trace.c tools/heapmap.c)
  target_include_directories(favo_cli PRIVATE tools)
  target_link_libraries(favo_cli PRIVATE favo_static)
  set_target_properties(favo_cli PROPERTIES OUTPUT_NAME favo)
endif()

if(FAVO_BUILD_TESTS)
  enable_testing()
  foreach(name basic policies stress threads)
    add_executable(test_${name} tests/test_${name}.c)
    target_include_directories(test_${name} PRIVATE tests src)
    target_link_libraries(test_${name} PRIVATE favo_static)
    add_test(NAME ${name} COMMAND test_${name})
  endforeach()
  if(UNIX AND NOT APPLE AND NOT FAVO_SANITIZE)
    add_test(NAME preload COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/preload.sh $<TARGET_FILE:favo_preload>)
  endif()
  if(FAVO_BUILD_TOOLS AND UNIX)
    add_test(NAME traces COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/traces.sh $<TARGET_FILE:favo_cli> ${CMAKE_CURRENT_SOURCE_DIR}/traces)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS favo favo_static EXPORT favo-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/favo.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TARGET favo_preload)
  install(TARGETS favo_preload LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(FAVO_BUILD_TOOLS)
  install(TARGETS favo_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(EXPORT favo-targets NAMESPACE favo:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/favo)
