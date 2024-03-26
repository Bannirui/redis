cmake_minimum_required(VERSION 3.28.0)
project(redis_6.2 C)

#[[
1 redis源码
2 前置依赖脚本
3 编译成可执行程序redis-server
4 依赖3rd库 libhiredis.a liblua.a libjemalloc.a
5 系统库
6 链接库
]]

#执行shell
execute_process(
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/src/mkreleasehdr.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src
)

#系统库 不同的平台依赖的系统库有差异
set(SYS_LIB -lm -ldl -lresolv -lpthread)
if (LINUX)
    set(SYS_LIB "${SYS_LIB} -latomic -lnsl -lrt -lcrypt -lbsd")
elseif (APPLE)
endif ()

#redis-server的源码文件
set(REDIS_SERVER_SRC
        src/adlist.c
        src/quicklist.c
        src/ae.c
        src/anet.c
        src/dict.c
        src/server.c
        src/sds.c
        src/zmalloc.c
        src/lzf_c.c
        src/lzf_d.c
        src/pqsort.c
        src/zipmap.c
        src/sha1.c
        src/ziplist.c
        src/release.c
        src/networking.c
        src/util.c
        src/object.c
        src/db.c
        src/replication.c
        src/rdb.c
        src/t_string.c
        src/t_list.c
        src/t_set.c
        src/t_zset.c
        src/t_hash.c
        src/config.c
        src/aof.c
        src/pubsub.c
        src/multi.c
        src/debug.c
        src/sort.c
        src/intset.c
        src/syncio.c
        src/cluster.c
        src/crc16.c
        src/endianconv.c
        src/slowlog.c
        src/scripting.c
        src/bio.c
        src/rio.c
        src/rand.c
        src/memtest.c
        src/crcspeed.c
        src/crc64.c
        src/bitops.c
        src/sentinel.c
        src/notify.c
        src/setproctitle.c
        src/blocked.c
        src/hyperloglog.c
        src/latency.c
        src/sparkline.c
        src/redis-check-rdb.c
        src/redis-check-aof.c
        src/geo.c
        src/lazyfree.c
        src/module.c
        src/evict.c
        src/expire.c
        src/geohash.c
        src/geohash_helper.c
        src/childinfo.c
        src/defrag.c
        src/siphash.c
        src/rax.c
        src/t_stream.c
        src/listpack.c
        src/localtime.c
        src/lolwut.c
        src/lolwut5.c
        src/lolwut6.c
        src/acl.c
        src/gopher.c
        src/tracking.c
        src/connection.c
        src/tls.c
        src/sha256.c
        src/timeout.c
        src/setcpuaffinity.c
        src/monotonic.c
        src/mt19937-64.c
)

#external 3rd库
add_subdirectory(deps)

#3rd头文件路径
set(DEPS_HEADER_PATH ${CMAKE_CURRENT_SOURCE_DIR}/deps)
include_directories(${DEPS_HEADER_PATH}/hdr_histogram)
include_directories(${DEPS_HEADER_PATH}/hiredis)
include_directories(${DEPS_HEADER_PATH}/jemalloc/include)
include_directories(${DEPS_HEADER_PATH}/linenoise)
include_directories(${DEPS_HEADER_PATH}/lua/src)

#3rd库路径
set(DEPS_LIB_PATH ${CMAKE_CURRENT_BINARY_DIR}/deps)
link_directories(${DEPS_LIB_PATH}/hdr_histogram)
link_directories(${DEPS_LIB_PATH}/hiredis)
link_directories(${DEPS_LIB_PATH}/jemalloc)
link_directories(${DEPS_LIB_PATH}/linenoise)
link_directories(${DEPS_LIB_PATH}/lua)

add_executable(redis-server ${REDIS_SERVER_SRC})

#链接库文件
target_link_libraries(redis-server
        hiredis
        lua
        ${SYS_LIB}
)

# jemalloc三方库只在linux上用
if (LINUX)
    target_link_libraries(redis-server
            jemalloc
    )
endif ()