cmake构建项目工程
---

### make的构建流程

- 根目录的Makefile

- src目录的Makefle

- ld链接 依赖REDIS_SERVER_OBJ libhiredis.a liblua.a FINAL_LIBS

- jemalloc

- FINAL_LIBS

  - -lm

  - -latomic 

  -	-ldl -lnsl -lsocket -lresolv -lpthread -lrt

  - -lcrypt -lbsd

  - -lexecinfo

- REDIS_SERVER_OBJ=adlist.o quicklist.o ae.o anet.o dict.o server.o sds.o zmalloc.o lzf_c.o lzf_d.o pqsort.o zipmap.o sha1.o ziplist.o release.o networking.o util.o object.o db.o replication.o rdb.o t_string.o t_list.o t_set.o t_zset.o t_hash.o config.o aof.o pubsub.o multi.o debug.o sort.o intset.o syncio.o cluster.o crc16.o endianconv.o slowlog.o scripting.o bio.o rio.o rand.o memtest.o crcspeed.o crc64.o bitops.o sentinel.o notify.o setproctitle.o blocked.o hyperloglog.o latency.o sparkline.o redis-check-rdb.o redis-check-aof.o geo.o lazyfree.o module.o evict.o expire.o geohash.o geohash_helper.o childinfo.o defrag.o siphash.o rax.o t_stream.o listpack.o localtime.o lolwut.o lolwut5.o lolwut6.o acl.o gopher.o tracking.o connection.o tls.o sha256.o timeout.o setcpuaffinity.o monotonic.o mt19937-64.o

### [cmake笔记](https://bannirui.github.io/2024/02/21/Redis-2%E5%88%B7-0x14-cmake%E6%9E%84%E5%BB%BA%E9%A1%B9%E7%9B%AE/)
