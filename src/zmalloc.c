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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including zmalloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void zlibc_free(void *ptr) {
    free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "config.h"
#include "zmalloc.h"
#include "atomicvar.h"

/**
 * PREFIX_SIZE是内存前缀 这个内存前缀是redis级别的 目的是为了知道redis从OS系统真实获得(或者近似真实获得)了多少内存
 * 本质就是redis通过PREFIX_SIZE模拟OS实现对内存块大小的记录 只不过受限于系统层面的级别无法准确而已 但是也够用了
 *   <li>对于已经有malloc_size支持的内存分配器 有直接库函数知道redis从OS获取到的真实内存大小 所以不需要redis额外负担记录的职责<li>
 *   <li>对于没有malloc_size redis只能自己负担这个记录工作 但是因为没有OS帮助 是不知道OS分配的真实内存大小 只能靠redis记下自己主动申请了多少空间</li>
 * 因此在第二种情况下 redis就要评估到底需要多大的额外空间来记录内存大小 这个值大小肯定依赖malloc接受的入参 又跟机器的字宽有直接关系
 * 但是不论怎样 肯定是一个unsigned的整数 并且PREFIX_SIZE肯定得>0
 * 此时redis期待向OS申请的内存大小就是sz+PREFIX_SIZE 两个unsigned整数相加就可能存在整数溢出的情况 如果溢出了那么两数求和就是负数
 *
 * 因此ASSERT_NO_SIZE_OVERFLOW断言的就是判断要向malloc申请的内存大小没有超过malloc能够分配的上限 即没有溢出
 */
#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
/* 内存分配器支持malloc_size 不需要redis自己负担前缀内存来记录大小 因此也size_t类型本身就是malloc函数接受的类型 直接传给malloc即可 即不需要对参数判断 因此断言函数是空的 */
#define ASSERT_NO_SIZE_OVERFLOW(sz)

#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
/* 内存分配器不支持malloc_size 需要redis自己负担前缀内存来记录大小 因此总共期待申请的内存大小是sz+PREFIX_SIZE 此时就要判断这个和是否溢出了size_t类型的整数大小 */
#define ASSERT_NO_SIZE_OVERFLOW(sz) assert((sz) + PREFIX_SIZE > (sz))

#endif

/* When using the libc allocator, use a minimum allocation size to match the
 * jemalloc behavior that doesn't return NULL in this case.
 */
#define MALLOC_MIN_SIZE(x) ((x) > 0 ? (x) : sizeof(long))

/* Explicitly override malloc/free etc when using tcmalloc. */
/**
 * malloc系列库函数的封装为zmalloc系列
 * 无非就3种情况
 * <ul>
 *   <li>tcmalloc</li>
 *   <li>jemalloc</li>
 *   <li>libc</li>
 * </ul>
 */
 /* tcmalloc分配器的函数封装 */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count,size) tc_calloc(count,size)
#define realloc(ptr,size) tc_realloc(ptr,size)
#define free(ptr) tc_free(ptr)
#elif defined(USE_JEMALLOC)
 /* jemalloc分配器的函数封装 */
#define malloc(size) je_malloc(size)
#define calloc(count,size) je_calloc(count,size)
#define realloc(ptr,size) je_realloc(ptr,size)
#define free(ptr) je_free(ptr)
#define mallocx(size,flags) je_mallocx(size,flags)
#define dallocx(ptr,flags) je_dallocx(ptr,flags)
#endif

 /* 原子更新使用内存 malloc申请完之后 加上新分配的空间大小 */
#define update_zmalloc_stat_alloc(__n) atomicIncr(used_memory,(__n))
/* 原子更新使用内存 free释放完之后 减去释放的空间大小 */
#define update_zmalloc_stat_free(__n) atomicDecr(used_memory,(__n))

 /**
  * redis侧记录使用的内存大小
  * <ul>
  *   <li>编译环境有malloc_size支持的情况下 记录的就是OS实实在在分配给redis的内存大小 也就是redis真实用了多少空间</li>
  *   <li>编译环境没有malloc_size支持的情况下 记录的就是redis实际向OS申请的内存大小 OS实际分配给redis的空间>=这个值 因此这种情况下记录的`使用空间`就是实际被分配的约数</li>
  * </ul>
  * 为什么要定义成原子变量
  * 我现在的认知是虽然redis的IO操作(即面向用户的存取操作)是单线程
  * 但是redis还会开启新线程处理后台任务或者非用户层的存取任务
  * 那么就可能面临并发更新这个内存值的情况
  */
static redisAtomic size_t used_memory = 0;

/**
 * 内存OOM处理器-默认处理器
 * 这个处理器实现相对简单
 * <ul>
 *   <li>打印报错信息</li>
 *   <li>终止当前进程</li>
 * </ul>
 * fflush(...)函数是个同步函数
 * 在终止进程之前fflush(...)函数的原因应该是保证在进程停止之前输出标准错误信息 防止数据呆在缓冲区没输出 然后进程结束了
 */
static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
        size);
    fflush(stderr);
    abort();
}

// 定义函数指针 指针变量指向的函数在发生OOM时被调用 则该函数就是名义上的OOM处理器
// 决定了发生内存OOM时怎么处理
static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

/* Try allocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
/**
 * 尝试动态内存分配
 * 分配失败就返回NULL
 *
 * 不处理OOM 关注内存块大小
 * @param size 期待申请的内存大小
 * @param usable 在内容申请成功的前提下 表示成功申请下来的内存块大小(设为n) 可能存在两种情况
 *        <ul>
 *          <li>n>=申请的大小</li>
 *          <li>n==申请的大小</li>
 *        </ul>
 * @return 可以使用的起始地址 用来存取用户数据
 */
void *ztrymalloc_usable(size_t size, size_t *usable) {
  /**
   * 这个断言函数我觉得真实精妙
   * 判定一个类型的数数否溢出的思路
   */
    ASSERT_NO_SIZE_OVERFLOW(size);
	/**
	 * 调用malloc通过内存分配器向OS申请内存
	 */
    void *ptr = malloc(MALLOC_MIN_SIZE(size)+PREFIX_SIZE);

    if (!ptr) return NULL;
#ifdef HAVE_MALLOC_SIZE
	// 有malloc_size库函数支持 直接获取申请到的内存块的实际大小 更新使用内存 加上新分配的大小
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
	// 内存块实际大小(该值>=申请的大小)
    if (usable) *usable = size;
	// malloc给的指针已经指向了给用户使用的起始地址
    return ptr;
#else
  // 没有有malloc_size库函数支持 更新使用内存 加上新申请的大小
  // 使用内存块的前缀空间记录这个内存块的大小(理论上应该记录实际大小 但是无从获取 因此只能记录期待申请的大小)
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
	// 内存块的近似大小(该值==申请的大小)
    if (usable) *usable = size;
	// 自己空出前缀空间 模拟malloc的实现 将指针指向给用户使用的起始地址
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/* Allocate memory or panic */
/**
 * 对malloc的封装
 * 如果OOM要对OOM进行处理
 *
 * 处理OOM 关注内存块大小
 */
void *zmalloc(size_t size) {
    // 尝试申请内存 并不关注获得的内存块大小
    void *ptr = ztrymalloc_usable(size, NULL);
	// 内存分配失败 回调处理器处理OOM
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
/**
 * 对malloc的封装
 * 尝试动态分配内存
 * 如果OOM了也直接交给上层关注
 *
 * 不处理OOM 不关注内存块大小
 */
void *ztrymalloc(size_t size) {
    void *ptr = ztrymalloc_usable(size, NULL);
    return ptr;
}

/* Allocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
/**
 * 对malloc的封装
 * 尝试动态分配内存 并获取到内存块大小
 * 如果OOM了也要进行处理
 *
 * 处理OOM 关注内存块大小
 */
void *zmalloc_usable(size_t size, size_t *usable) {
    void *ptr = ztrymalloc_usable(size, usable);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Allocation and free functions that bypass the thread cache
 * and go straight to the allocator arena bins.
 * Currently implemented only for jemalloc. Used for online defragmentation. */
#ifdef HAVE_DEFRAG
void *zmalloc_no_tcache(size_t size) {
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = mallocx(size+PREFIX_SIZE, MALLOCX_TCACHE_NONE);
    if (!ptr) zmalloc_oom_handler(size);
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
}

void zfree_no_tcache(void *ptr) {
    if (ptr == NULL) return;
    update_zmalloc_stat_free(zmalloc_size(ptr));
    dallocx(ptr, MALLOCX_TCACHE_NONE);
}
#endif

/* Try allocating memory and zero it, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
void *ztrycalloc_usable(size_t size, size_t *usable) {
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = calloc(1, MALLOC_MIN_SIZE(size)+PREFIX_SIZE);
    if (ptr == NULL) return NULL;

#ifdef HAVE_MALLOC_SIZE
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return ptr;
#else
    *((size_t*)ptr) = size;
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    if (usable) *usable = size;
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/* Allocate memory and zero it or panic */
void *zcalloc(size_t size) {
    void *ptr = ztrycalloc_usable(size, NULL);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
void *ztrycalloc(size_t size) {
    void *ptr = ztrycalloc_usable(size, NULL);
    return ptr;
}

/* Allocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zcalloc_usable(size_t size, size_t *usable) {
    void *ptr = ztrycalloc_usable(size, usable);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try reallocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable) {
    ASSERT_NO_SIZE_OVERFLOW(size);
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    /* not allocating anything, just redirect to free. */
	/**
	 * 边界处理
	 * 当前有可用内存 给定的要新申请的大小为0
	 * 这样的语义是释放已有的内存
	 */
    if (size == 0 && ptr != NULL) {
        zfree(ptr);
        if (usable) *usable = 0;
        return NULL;
    }
    /* Not freeing anything, just redirect to malloc. */
	/**
	 * 边界处理
	 * 当前持有的内存为空
	 * 这样的语义是新申请一片内存 不做初始化
	 */
    if (ptr == NULL)
        return ztrymalloc_usable(size, usable);

#ifdef HAVE_MALLOC_SIZE
	// 已经持有的内存块大小
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (newptr == NULL) {
        if (usable) *usable = 0;
        return NULL;
    }

	// 更新使用内存量
    update_zmalloc_stat_free(oldsize);
    size = zmalloc_size(newptr);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (newptr == NULL) {
        if (usable) *usable = 0;
        return NULL;
    }

    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/* Reallocate memory and zero it or panic */
/**
 * @brief 将ptr指向的数组扩容到size字节大小 成功后返回的指针依然是ptr
 *        新数组相当于对旧数据的空间追加 包含了旧数组的所有元素数据
 *        新追加的内存上全部写0
 * @param ptr 指向要扩容的数组
 * @param size 要扩容到多大字节
 */
void *zrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable(ptr, size, NULL);
    if (!ptr && size != 0) zmalloc_oom_handler(size);
    return ptr;
}

/* Try Reallocating memory, and return NULL if failed. */
void *ztryrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable(ptr, size, NULL);
    return ptr;
}

/* Reallocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zrealloc_usable(void *ptr, size_t size, size_t *usable) {
    ptr = ztryrealloc_usable(ptr, size, usable);
    if (!ptr && size != 0) zmalloc_oom_handler(size);
    return ptr;
}

/* Provide zmalloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store a header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    return size+PREFIX_SIZE;
}
size_t zmalloc_usable_size(void *ptr) {
    return zmalloc_size(ptr)-PREFIX_SIZE;
}
#endif

/**
 * 对free(...)函数的封装
 * <ul>
 *   <li>更新内存使用量 要释放的内存块对应的内存块大小要扣减掉</li>
 *   <li>向OS申请释放内存</li>
 * </ul>
 *
 * 要更新内存使用量 就要知道内存块大小 此时也更加印证了PREFIX_SIZE的重要性 正式因为PREFIX_SIZE统一了跟OS系统一样的内存块大小机制 大大简化了整个的内存使用量统计操作
 */
void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
	// 更新内存使用量 释放掉的内存量
    update_zmalloc_stat_free(zmalloc_size(ptr));
	// 释放
    free(ptr);
#else
	// 前移前缀空间 这个地方才是OS视角的use_ptr
    realptr = (char*)ptr-PREFIX_SIZE;
	// 前缀空间上存储的内存大小
    oldsize = *((size_t*)realptr);
	// 更新内存使用量 释放掉的内存量
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
	// 释放
    free(realptr);
#endif
}

/* Similar to zfree, '*usable' is set to the usable size being freed. */
// 同zfree 只是将待释放的内存块大小返回出来
void zfree_usable(void *ptr, size_t *usable) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(*usable = zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    *usable = oldsize = *((size_t*)realptr);
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

/**
 * 复制字符串
 * 实现很简单 但是我不太理解为啥把这个函数放在zmalloc
 */
char *zstrdup(const char *s) {
    // 字符串长度 多一个byte放\0结束符
    size_t l = strlen(s)+1;
    char *p = zmalloc(l);

    memcpy(p,s,l);
    return p;
}

// 内存使用量
size_t zmalloc_used_memory(void) {
    size_t um;
	// 读取used_memory变量的值
    atomicGet(used_memory,um);
    return um;
}

/**
 * 注册内存OOM回调函数
 * @param oom_handler 函数指针 发生OOM时的处理器
 */
void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
    zmalloc_oom_handler = oom_handler;
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * For this kind of "fast RSS reporting" usages use instead the
 * function RedisEstimateRSS() that is a much faster (and less precise)
 * version of the function. */

/**
 * linux系统的/proc虚拟文件系统
 * 在linux上想要知道某个进程的实际内存占用很简单 使用top命令即可 其中RES就是内存驻留(不包含swap交换内存) 单位是kb
 * 具体到某个进程的运行时信息存储在文件/proc/{pid}/stat上 这个文件内容每个指标项通过空格作为分割符 第24个就是RSS指标 单位是页
 * 因此linux系统某个进程的RSS就是直接读这个文件即可
 */
#if defined(HAVE_PROC_STAT)
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void) {
    // 页大小(单位是bytes) 下面要去读/proc/{pid}/stat获取RSS大小 得到的是页数
	// 结果是4096 byte
    int page = sysconf(_SC_PAGESIZE);
    size_t rss;
    char buf[4096];
    char filename[256];
    int fd, count;
    char *p, *x;
	// 虚拟文件系统的文件路径/proc/{pid}/stat
    snprintf(filename,256,"/proc/%ld/stat",(long) getpid());
	// 文件不存在
    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
	// 文件内容读到buf数组中 读完关闭文件
    if (read(fd,buf,4096) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);

    p = buf;
	// 各个指标用空格作为分割符 第24个就是RSS指标
    count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
    while(p && count--) {
        p = strchr(p,' ');
        if (p) p++;
    }
    if (!p) return 0;
	// 找到第24项和25项之间的空格 人为将其改成\0结束符 目的是让函数strtoll(...)将第24项字符串形式转成整数形式
    x = strchr(p,' ');
    if (!x) return 0;
    *x = '\0';
	// 将第24项的RSS字符串形式转换成10进制整数形式(long long类型)
    rss = strtoll(p,NULL,10);
	// rss页*每页page个byte
    rss *= page;
    return rss;
}

/* 苹果系统依赖的是微内核mach的实现 但是我没有找到对应的手册说明 也就无从谈起解读 只能说如果可以学习redis 如果有一天自己要在mac上获取进程的RSS信息 可以用它的方式 */
#elif defined(HAVE_TASKINFO)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void) {
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}

#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

size_t zmalloc_get_rss(void) {
    struct kinfo_proc info;
    size_t infolen = sizeof(info);
    int mib[4];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    if (sysctl(mib, 4, &info, &infolen, NULL, 0) == 0)
#if defined(__FreeBSD__)
        return (size_t)info.ki_rssize * getpagesize();
#else
        return (size_t)info.kp_vm_rssize * getpagesize();
#endif

    return 0L;
}

#elif defined(__NetBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>

size_t zmalloc_get_rss(void) {
    struct kinfo_proc2 info;
    size_t infolen = sizeof(info);
    int mib[6];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();
    mib[4] = sizeof(info);
    mib[5] = 1;
    if (sysctl(mib, 4, &info, &infolen, NULL, 0) == 0)
        return (size_t)info.p_vm_rssize * getpagesize();

    return 0L;
}

#elif defined(HAVE_PSINFO)
#include <unistd.h>
#include <sys/procfs.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void) {
    struct prpsinfo info;
    char filename[256];
    int fd;

    snprintf(filename,256,"/proc/%ld/psinfo",(long) getpid());

    if ((fd = open(filename,O_RDONLY)) == -1) return 0;
    if (ioctl(fd, PIOCPSINFO, &info) == -1) {
        close(fd);
	return 0;
    }

    close(fd);
    return info.pr_rssize;
}

#else
size_t zmalloc_get_rss(void) {
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

/**
 * 从内存分配器端获取内存相关的指标
 * <li>resident</li>
 * <li>active</li>
 * <li>allocated</li>
 * 使用了jemalloc分配器的可以直接从jemalloc提供的接口获取
 * 其他的分配器实现没有这个功能
 */

/* 使用了jemalloc分配器 从接口获取数据 */
#if defined(USE_JEMALLOC)

/**
 * 通过Makefile的跟踪和https://github.com/jemalloc/jemalloc/blob/dev/INSTALL.md
 * 知道的是jemalloc提供的系列函数都指定了je_作为前缀
 * 那么反推这里的je_mallctl就是mallctl的实现
 * 所以只要搞明白mallctl的作用即可
 * mallctl(name, oldp, oldlenp, newp, newlen)
 * 即mallctl会将要查询的name的结果放到oldp指针变量
 */
int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident) {
    uint64_t epoch = 1;
    size_t sz;
    *allocated = *resident = *active = 0;
    /* Update the statistics cached by mallctl. */
    sz = sizeof(epoch);
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);
    sz = sizeof(size_t);
    /* Unlike RSS, this does not include RSS from shared libraries and other non
     * heap mappings. */
    je_mallctl("stats.resident", resident, &sz, NULL, 0);
    /* Unlike resident, this doesn't not include the pages jemalloc reserves
     * for re-use (purge will clean that). */
    je_mallctl("stats.active", active, &sz, NULL, 0);
    /* Unlike zmalloc_used_memory, this matches the stats.resident by taking
     * into account all allocations done by this process (not only zmalloc). */
    je_mallctl("stats.allocated", allocated, &sz, NULL, 0);
    return 1;
}

void set_jemalloc_bg_thread(int enable) {
    /* let jemalloc do purging asynchronously, required when there's no traffic 
     * after flushdb */
    char val = !!enable;
    je_mallctl("background_thread", NULL, 0, &val, 1);
}

int jemalloc_purge() {
    /* return all unused (reserved) pages to the OS */
    char tmp[32];
    unsigned narenas = 0;
    size_t sz = sizeof(unsigned);
    if (!je_mallctl("arenas.narenas", &narenas, &sz, NULL, 0)) {
        sprintf(tmp, "arena.%d.purge", narenas);
        if (!je_mallctl(tmp, NULL, 0, NULL, 0))
            return 0;
    }
    return -1;
}

/**
 * 内存分配器不是jemalloc 没有查询接口支持
 * <li>allocated</li>
 * <li>active</li>
 * <li>resident</li>
 * 这3个值给定0 在使用的时候以0为判定分支
 */
#else

int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident) {
    *allocated = *resident = *active = 0;
    return 1;
}

void set_jemalloc_bg_thread(int enable) {
    ((void)(enable));
}

int jemalloc_purge() {
    return 0;
}

#endif

#if defined(__APPLE__)
/* For proc_pidinfo() used later in zmalloc_get_smap_bytes_by_field().
 * Note that this file cannot be included in zmalloc.h because it includes
 * a Darwin queue.h file where there is a "LIST_HEAD" macro (!) defined
 * conficting with Redis user code. */
#include <libproc.h>
#endif

/* Get the sum of the specified field (converted form kb to bytes) in
 * /proc/self/smaps. The field must be specified with trailing ":" as it
 * apperas in the smaps output.
 *
 * If a pid is specified, the information is extracted for such a pid,
 * otherwise if pid is -1 the information is reported is about the
 * current process.
 *
 * Example: zmalloc_get_smap_bytes_by_field("Rss:",-1);
 */
#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid) {
    char line[1024];
    size_t bytes = 0;
    int flen = strlen(field);
    FILE *fp;

    if (pid == -1) {
        fp = fopen("/proc/self/smaps","r");
    } else {
        char filename[128];
        snprintf(filename,sizeof(filename),"/proc/%ld/smaps",pid);
        fp = fopen(filename,"r");
    }

    if (!fp) return 0;
    while(fgets(line,sizeof(line),fp) != NULL) {
        if (strncmp(line,field,flen) == 0) {
            char *p = strchr(line,'k');
            if (p) {
                *p = '\0';
                bytes += strtol(line+flen,NULL,10) * 1024;
            }
        }
    }
    fclose(fp);
    return bytes;
}
#else
/* Get sum of the specified field from libproc api call.
 * As there are per page value basis we need to convert
 * them accordingly.
 *
 * Note that AnonHugePages is a no-op as THP feature
 * is not supported in this platform
 */
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid) {
#if defined(__APPLE__)
    struct proc_regioninfo pri;
    if (pid == -1) pid = getpid();
    if (proc_pidinfo(pid, PROC_PIDREGIONINFO, 0, &pri,
                     PROC_PIDREGIONINFO_SIZE) == PROC_PIDREGIONINFO_SIZE)
    {
        int pagesize = getpagesize();
        if (!strcmp(field, "Private_Dirty:")) {
            return (size_t)pri.pri_pages_dirtied * pagesize;
        } else if (!strcmp(field, "Rss:")) {
            return (size_t)pri.pri_pages_resident * pagesize;
        } else if (!strcmp(field, "AnonHugePages:")) {
            return 0;
        }
    }
    return 0;
#endif
    ((void) field);
    ((void) pid);
    return 0;
}
#endif

/* Return the total number bytes in pages marked as Private Dirty.
 *
 * Note: depending on the platform and memory footprint of the process, this
 * call can be slow, exceeding 1000ms!
 */
size_t zmalloc_get_private_dirty(long pid) {
    return zmalloc_get_smap_bytes_by_field("Private_Dirty:",pid);
}

/* Returns the size of physical memory (RAM) in bytes.
 * It looks ugly, but this is the cleanest way to achieve cross platform results.
 * Cleaned up from:
 *
 * http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
 *
 * Note that this function:
 * 1) Was released under the following CC attribution license:
 *    http://creativecommons.org/licenses/by/3.0/deed.en_US.
 * 2) Was originally implemented by David Robert Nadeau.
 * 3) Was modified for Redis by Matt Stancliff.
 * 4) This note exists in order to comply with the original license.
 */
size_t zmalloc_get_memory_size(void) {
#if defined(__unix__) || defined(__unix) || defined(unix) || \
    (defined(__APPLE__) && defined(__MACH__))
#if defined(CTL_HW) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM64))
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
    mib[1] = HW_MEMSIZE;            /* OSX. --------------------- */
#elif defined(HW_PHYSMEM64)
    mib[1] = HW_PHYSMEM64;          /* NetBSD, OpenBSD. --------- */
#endif
    int64_t size = 0;               /* 64-bit */
    size_t len = sizeof(size);
    if (sysctl( mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L;          /* Failed? */

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    /* FreeBSD, Linux, OpenBSD, and Solaris. -------------------- */
    return (size_t)sysconf(_SC_PHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE);

#elif defined(CTL_HW) && (defined(HW_PHYSMEM) || defined(HW_REALMEM))
    /* DragonFly BSD, FreeBSD, NetBSD, OpenBSD, and OSX. -------- */
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_REALMEM)
    mib[1] = HW_REALMEM;        /* FreeBSD. ----------------- */
#elif defined(HW_PHYSMEM)
    mib[1] = HW_PHYSMEM;        /* Others. ------------------ */
#endif
    unsigned int size = 0;      /* 32-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L;          /* Failed? */
#else
    return 0L;          /* Unknown method to get the data. */
#endif
#else
    return 0L;          /* Unknown OS. */
#endif
}

#ifdef REDIS_TEST
#define UNUSED(x) ((void)(x))
int zmalloc_test(int argc, char **argv, int accurate) {
    void *ptr;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);
    printf("Malloc prefix size: %d\n", (int) PREFIX_SIZE);
    printf("Initial used memory: %zu\n", zmalloc_used_memory());
    ptr = zmalloc(123);
    printf("Allocated 123 bytes; used: %zu\n", zmalloc_used_memory());
    ptr = zrealloc(ptr, 456);
    printf("Reallocated to 456 bytes; used: %zu\n", zmalloc_used_memory());
    zfree(ptr);
    printf("Freed pointer; used: %zu\n", zmalloc_used_memory());
    return 0;
}
#endif
