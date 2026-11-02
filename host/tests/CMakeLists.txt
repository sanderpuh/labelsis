set(QL700_PROTOCOL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../components/ql700_protocol")
set(QL700_INCLUDE_DIR "${QL700_PROTOCOL_DIR}/include")
set(QL700_SOURCES
    "${QL700_PROTOCOL_DIR}/src/ql700_status.c"
    "${QL700_PROTOCOL_DIR}/src/ql700_raster.c")

foreach(test_name IN ITEMS ql700_status ql700_raster ql700_bitmap)
    add_executable(test_${test_name}
        "${CMAKE_CURRENT_LIST_DIR}/../../test/${test_name}_test.c"
        ${QL700_SOURCES})
    target_include_directories(test_${test_name} PRIVATE "${QL700_INCLUDE_DIR}")
    target_compile_features(test_${test_name} PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(test_${test_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()
