/* zmalloc - total amount of allocated memory aware version of malloc()
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __ZMALLOC_H
#define __ZMALLOC_H

/* Double expansion needed for stringification of macro values. */
#define __xstr(s) __str(s)
#define __str(s) #s

/**
 * 使用哪个内存分配器是在编译的时候通过-D宏参数定义的
 * <ul>
 *   <li>-DUSE_TCMALLOC 使用tcmalloc分配器</li>
 *   <li>-DUSE_JEMALLOC 使用jemalloc分配器</li>
 *   <li>没有指定上述二者 就使用libc 即系统自带的默认分配器实现</li>
 * </ul>
 */
/**
 * 编译参数-DUSE_TCMALLOC指定了使用tcmalloc分配器
 * 该分配器实现并不是每个版本都支持malloc_size
 * <ul>
 *   <li>版本<1.6 不支持malloc_size</li>
 *   <li>版本>=1.6 支持malloc_size</li>
 * </ul>
 * 将tc_malloc_size(...)封装成zmalloc_size(...)
 */
#if defined(USE_TCMALLOC)
#define ZMALLOC_LIB ("tcmalloc-" __xstr(TC_VERSION_MAJOR) "." __xstr(TC_VERSION_MINOR))
/* tcmalloc头文件 */
#include <google/tcmalloc.h>
#if (TC_VERSION_MAJOR == 1 && TC_VERSION_MINOR >= 6) || (TC_VERSION_MAJOR > 1)
/* tcmalloc版本>=1.6支持malloc_size 将其提供的tc_malloc_size(...)函数封装为统一的zmalloc_size(...) */
#define HAVE_MALLOC_SIZE 1
/* zmalloc_size(...)封装 */
#define zmalloc_size(p) tc_malloc_size(p)
#else
/* tcmaloc版本<1.6不支持malloc_size */
#error "Newer version of tcmalloc required"
#endif

/**
 * 编译参数-DUSE_JEMALLOC指定了使用jemalloc分配器
 * 该分配器实现并不是每个版本都支持malloc_size
 * <ul>
 *   <li>版本<2.1 不支持malloc_size</li>
 *   <li>版本>=2.1 支持malloc_size</li>
 * </ul>
 * 将je_malloc_usable_size(...)封装成zmalloc_size(...)
 */
#elif defined(USE_JEMALLOC)
#define ZMALLOC_LIB ("jemalloc-" __xstr(JEMALLOC_VERSION_MAJOR) "." __xstr(JEMALLOC_VERSION_MINOR) "." __xstr(JEMALLOC_VERSION_BUGFIX))
/* jemalloc头文件 */
#include <jemalloc/jemalloc.h>
#if (JEMALLOC_VERSION_MAJOR == 2 && JEMALLOC_VERSION_MINOR >= 1) || (JEMALLOC_VERSION_MAJOR > 2)
/* jemalloc版本>=2.1支持malloc_size 将其提供的je_malloc_size(...)函数封装为统一的zmalloc_size(...) */
#define HAVE_MALLOC_SIZE 1
/* zmalloc_size(...)封装 */
#define zmalloc_size(p) je_malloc_usable_size(p)
#else
/* jemalloc版本<2.1不支持malloc_size */
#error "Newer version of jemalloc required"
#endif

/**
 * 没有通过编译参数指定具体的内存分配器
 * 就用系统libc自带的默认实现
 * 我常用的系统就macos和linux这两个 二者对c标准库的实现不同
 * <ul>
 *   <li>头文件
 *     <ul>
 *       <li>mac头文件为<malloc/malloc.h></li>
 *       <li>linux头文件为<malloc.h></li>
 *     </ul>
 *   </li>
 *   <li>malloc_size的支持
 *     <ul>
 *       <li>mac的库函数为malloc_size(...)</li>
 *       <li>linux的库函数为malloc_usable_size(...)</li>
 *     </ul>
 *   </li>
 * </ul>
 */
#elif defined(__APPLE__)
/**
 * mac平台
 * <ul>
 *   <li>malloc系列头文件为<malloc/malloc.h></li>
 *   <li>有malloc_size支持</li>
 *   <li>malloc_size实现为malloc_size(...)，将其封装为zmalloc_size(...)</li>
 * </ul>
 */
 /* mac下libc头文件 */
#include <malloc/malloc.h>
#define HAVE_MALLOC_SIZE 1
/* zmalloc_size(...)函数封装 */
#define zmalloc_size(p) malloc_size(p)
#endif

/* On native libc implementations, we should still do our best to provide a
 * HAVE_MALLOC_SIZE capability. This can be set explicitly as well:
 *
 * NO_MALLOC_USABLE_SIZE disables it on all platforms, even if they are
 *      known to support it.
 * USE_MALLOC_USABLE_SIZE forces use of malloc_usable_size() regardless
 *      of platform.
 */
#ifndef ZMALLOC_LIB
#define ZMALLOC_LIB "libc"

#if !defined(NO_MALLOC_USABLE_SIZE) && \
    (defined(__GLIBC__) || defined(__FreeBSD__) || \
     defined(USE_MALLOC_USABLE_SIZE))

/* Includes for malloc_usable_size() */
#ifdef __FreeBSD__
#include <malloc_np.h>
#else
/**
 * linux平台的libc实现为glibc
 * <ul>
 *   <li>malloc系列头文件为<malloc.h></li>
 *   <li>有malloc_size支持</li>
 *   <li>malloc_size实现为malloc_usable_size(...)，将其封装为zmalloc_size(...)</li>
 * </ul>
 */
/* linux下libc头文件 */
#include <malloc.h>
#endif

#define HAVE_MALLOC_SIZE 1
/* zmalloc_size(...)函数封装 */
#define zmalloc_size(p) malloc_usable_size(p)

#endif
#endif

/* We can enable the Redis defrag capabilities only if we are using Jemalloc
 * and the version used is our special version modified for Redis having
 * the ability to return per-allocation fragmentation hints. */
#if defined(USE_JEMALLOC) && defined(JEMALLOC_FRAG_HINT)
#define HAVE_DEFRAG
#endif

// 处理OOM 不关注内存块大小
void *zmalloc(size_t size);
// 处理OOM 不关注内存块大小
void *zcalloc(size_t size);
// 处理OOM 不关注内存块大小
void *zrealloc(void *ptr, size_t size);

// 不处理OOM 不关注内存块大小
void *ztrymalloc(size_t size);
// 不处理OOM 不关注内存块大小
void *ztrycalloc(size_t size);
// 不处理OOM 不关注内存块大小
void *ztryrealloc(void *ptr, size_t size);

void zfree(void *ptr);

// 处理OOM 关注内存块大小
void *zmalloc_usable(size_t size, size_t *usable);
// 处理OOM 关注内存块大小
void *zcalloc_usable(size_t size, size_t *usable);
// 处理OOM 关注内存块大小
void *zrealloc_usable(void *ptr, size_t size, size_t *usable);

// 不处理OOM 关注内存块大小
void *ztrymalloc_usable(size_t size, size_t *usable);
// 不处理OOM 关注内存块大小
void *ztrycalloc_usable(size_t size, size_t *usable);
// 不处理OOM 关注内存块大小
void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable);

void zfree_usable(void *ptr, size_t *usable);
char *zstrdup(const char *s);
size_t zmalloc_used_memory(void);
void zmalloc_set_oom_handler(void (*oom_handler)(size_t));

/**
 * RSS=Resident Set Size 当前进程驻留在内存中的空间大小 不包括被swap出去的空间
 * 申请的内存空间不会常驻内存 系统会将暂时不用的空间从内存中置换到swap区
 * 即这个函数返回的是进程实际消耗的物理内存空间
 * 返回值的单位是byte
 *
 * 每个平台实现又不一样 因此要根据不同的平台进行不同的实现
 */
size_t zmalloc_get_rss(void);

/**
 * 这个函数的实现以是否使用jemalloc作为了分支判断
 * <ul>用来获取内存分配器记录的内存相关的指标信息
 *   <li>stats.resident</li>
 *   <li>stats.active</li>
 *   <li>stats.allocated</li>
 * </ul>
 * 为什么用是否使用jemalloc分配器作为判断分支呢
 * 说明这些信息只能从jemalloc分配器获取
 * 其他分配器 包括tcmalloc和libc都没有提供相关接口
 * 内存相关的数据指标获取一定是计算场景需要 那么不是jemalloc分配器又该怎么处理呢 所以到时候肯定会判断
 * <li>resident</li>
 * <li>active</li>
 * <li>allocated</li>
 * 这些值是否等于0 不等于0的走一个逻辑 等于0的走另一个兜底逻辑
 */
int zmalloc_get_allocator_info(size_t *allocated, size_t *active, size_t *resident);
void set_jemalloc_bg_thread(int enable);
int jemalloc_purge();
size_t zmalloc_get_private_dirty(long pid);
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid);
size_t zmalloc_get_memory_size(void);
void zlibc_free(void *ptr);

#ifdef HAVE_DEFRAG
void zfree_no_tcache(void *ptr);
void *zmalloc_no_tcache(size_t size);
#endif

#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr);
size_t zmalloc_usable_size(void *ptr);
#else
#define zmalloc_usable_size(p) zmalloc_size(p)
#endif

#ifdef REDIS_TEST
int zmalloc_test(int argc, char **argv, int accurate);
#endif

#endif /* __ZMALLOC_H */
