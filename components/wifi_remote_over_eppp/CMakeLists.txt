if(NOT CONFIG_ESP_WIFI_REMOTE_LIBRARY_EPPP AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register()
    return()
endif()

set(sources src/wifi_remote_rpc_client.cpp src/wifi_remote_rpc_server.cpp src/eppp_init.c)

idf_component_register(SRCS ${sources}
                       PRIV_INCLUDE_DIRS src
                       PRIV_REQUIRES esp_event esp_netif eppp_link esp-tls esp_wifi vfs)

include(CheckCXXCompilerFlag)
# -freflection is only accepted together with -std=c++26 / gnu++26
set(_wifi_rmt_saved_cmake_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=gnu++26")
check_cxx_compiler_flag(-freflection WIFI_RMT_HAS_FREFLECTION)
set(CMAKE_REQUIRED_FLAGS "${_wifi_rmt_saved_cmake_required_flags}")
unset(_wifi_rmt_saved_cmake_required_flags)
if(WIFI_RMT_HAS_FREFLECTION)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++26>
        $<$<COMPILE_LANGUAGE:CXX>:-freflection>
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_RMT_HAS_REFLECTION=1)
endif()
