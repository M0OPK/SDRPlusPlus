cmake_minimum_required(VERSION 3.13)
project(yaesu_cat_client)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(yaesu_cat_client PRIVATE "src/")
target_include_directories(yaesu_cat_client PRIVATE "../../decoder_modules/radio/src")
target_include_directories(yaesu_cat_client PRIVATE "../../core/async_comm/include/")

target_link_libraries(yaesu_cat_client PRIVATE async_comm)
