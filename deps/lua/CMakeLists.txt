file(GLOB LUA_SRC ./src/*.c)

#生成静态库liblua.a
add_library(lua ${LUA_SRC})
