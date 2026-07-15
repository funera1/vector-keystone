set(eapp_bin latency)
set(eapp_src eapp/latency.c)
set(host_bin latency-runner)
set(host_src host/host.cpp)
set(package_name "latency.ke")
set(package_script "./latency-runner latency eyrie-rt loader.bin")
set(eyrie_plugins "io_syscall linux_syscall env_setup")

# eapp

add_executable(${eapp_bin} ${eapp_src})
target_link_libraries(${eapp_bin} "-static" ${KEYSTONE_LIB_EAPP} ${KEYSTONE_LIB_EDGE})

target_include_directories(${eapp_bin}
  PUBLIC ${KEYSTONE_SDK_DIR}/include/app
  PUBLIC ${KEYSTONE_SDK_DIR}/include/edge)
  
# host

# Find the NUMA library

find_path(NUMA_INCLUDE_DIR numa.h)
find_library(NUMA_LIB numa)

if(NOT NUMA_INCLUDE_DIR OR NOT NUMA_LIB)
    message(FATAL_ERROR "NUMA library or headers not found")
endif()



add_executable(${host_bin} ${host_src})
target_include_directories(${host_bin} PRIVATE ${NUMA_INCLUDE_DIR})
target_link_libraries(${host_bin} ${KEYSTONE_LIB_HOST} ${KEYSTONE_LIB_EDGE} ${NUMA_LIB})


# add target for Eyrie runtime (see keystone.cmake)

set(eyrie_files_to_copy .options_log eyrie-rt loader.bin)
add_eyrie_runtime(${eapp_bin}-eyrie
  ${eyrie_plugins}
  ${eyrie_files_to_copy})

# add target for packaging (see keystone.cmake)

add_keystone_package(${eapp_bin}-package
  ${package_name}
  ${package_script}
  ${eyrie_files_to_copy} ${eapp_bin} ${host_bin})

add_dependencies(${eapp_bin}-package ${eapp_bin}-eyrie)

# add package to the top-level target
add_dependencies(examples ${eapp_bin}-package)
