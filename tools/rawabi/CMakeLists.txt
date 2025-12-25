cmake_minimum_required(VERSION 3.16)

set(RAWABI_SOURCES
  src/rawabi_common.cpp
  src/rawabi_generator.cpp
  src/rawabi_network.cpp
  src/rawabi_preview.cpp
)

find_package(Threads REQUIRED)

add_library(rawabi_common ${RAWABI_SOURCES})
target_include_directories(rawabi_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(rawabi_common PUBLIC m Threads::Threads)

find_package(OpenCV QUIET)
if(OpenCV_FOUND)
  target_compile_definitions(rawabi_common PUBLIC RAWABI_HAVE_OPENCV)
  target_link_libraries(rawabi_common PUBLIC ${OpenCV_LIBS})
endif()

add_executable(rawabi_sender src/rawabi_sender.cpp)
target_link_libraries(rawabi_sender PRIVATE rawabi_common)

add_executable(rawabi_receiver src/rawabi_receiver.cpp)
target_link_libraries(rawabi_receiver PRIVATE rawabi_common)

add_executable(rawabi_record src/rawabi_record.cpp)
target_link_libraries(rawabi_record PRIVATE rawabi_common)

add_executable(rawabi_playback src/rawabi_playback.cpp)
target_link_libraries(rawabi_playback PRIVATE rawabi_common)

install(TARGETS rawabi_sender rawabi_receiver rawabi_record rawabi_playback
        RUNTIME DESTINATION bin)
