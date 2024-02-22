#[[
cmake执行shell脚本 build_jemalloc.sh
sh脚本负责通过configure生成Makefile 再通过make进行编译静态库
]]
execute_process(
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/build_jemalloc.sh"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

#编译好的库文件移动到build构建目录
file(COPY lib/libjemalloc.a DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
file(REMOVE lib/libjemalloc.a)
