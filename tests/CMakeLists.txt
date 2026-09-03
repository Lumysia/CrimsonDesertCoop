add_executable(cdcoop_packet_tests
    packet_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
)

target_include_directories(cdcoop_packet_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

add_test(NAME cdcoop_packet_tests COMMAND cdcoop_packet_tests)
