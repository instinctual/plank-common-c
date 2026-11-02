find_package(Threads REQUIRED)

# Header-only checks: no common-C runtime or transport library is linked.
foreach(language c cpp)
  set(abi_test "plank-clipboard-abi-${language}")
  add_executable(${abi_test} "clipboard-wire-abi.${language}")
  target_include_directories(${abi_test} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${PLANK_TRANSPORT_DIR}/include")
  set_target_properties(${abi_test} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if (MSVC)
    target_compile_options(${abi_test} PRIVATE /W4 /WX)
  else()
    target_compile_options(${abi_test} PRIVATE -Wall -Wextra -Werror -pedantic)
  endif()
  add_test(NAME ${abi_test} COMMAND ${abi_test})
  set_tests_properties(${abi_test} PROPERTIES TIMEOUT 20)
endforeach()

function(add_plank_input_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE moonlight-common-c Threads::Threads)
  target_include_directories(${name} PRIVATE "${PLANK_TRANSPORT_DIR}/include")
  if (NOT WIN32)
    target_link_libraries(${name} PRIVATE m)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 20)
endfunction()

add_plank_input_test(plank-native-input-wire native-input-wire.c)
add_plank_input_test(plank-native-input-bounds native-input-bounds.c)
add_plank_input_test(plank-native-input-backpressure native-input-backpressure.c)
add_plank_input_test(plank-queue-ownership queue-ownership.c)
add_plank_input_test(plank-audio-timestamps audio-timestamps.c)

# GNU/LLVM ELF link wrapping supplies deterministic scheduler boundaries with
# no production hooks. Static linking is required to wrap worker calls.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  get_target_property(common_library_type moonlight-common-c TYPE)
  if (common_library_type STREQUAL "STATIC_LIBRARY")
    add_plank_input_test(plank-native-input-replacement native-input-replacement.c)
    set_property(TARGET plank-native-input-replacement APPEND_STRING PROPERTY LINK_FLAGS
      " -Wl,--wrap=LbqPollQueueElementIf")
  endif()
endif()
