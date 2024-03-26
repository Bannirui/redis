add_subdirectory(hdr_histogram)
#hiredis是cmake项目
add_subdirectory(hiredis)
#cmake执行shell脚本
# jemalloc只在linux才编译
if (LINUX)
    add_subdirectory(jemalloc)
endif ()
add_subdirectory(linenoise)
add_subdirectory(lua)
