cmake_minimum_required(VERSION 3.13)
project(icom_civ_client)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(icom_civ_client PRIVATE "src/")
target_include_directories(icom_civ_client PRIVATE "../../decoder_modules/radio/src")

target_link_libraries(icom_civ_client PRIVATE async_comm)
