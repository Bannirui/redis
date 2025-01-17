/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
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
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include "sds.h"
#include "sdsalloc.h"

const char *SDS_NOINIT = "SDS_NOINIT";

/**
 * 对于sds的5种类型 结构体的最后一个成员是柔性数组 因此占0空间
 * 所以本质sdshdr?就是sds字符串的头
 * @param type sds字符串类型枚举值
 * @return sds字符串类型的header大小多少个byte
 */
static inline int sdsHdrSize(char type) {
	/**
     * sds类型枚举用1个byte表示 高5位预留 低3位用于标识
	 * 用0b111即0x07 也就是十进制的7把flags的低3位取出来
	 */
    switch(type&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return sizeof(struct sdshdr5);
        case SDS_TYPE_8:
            return sizeof(struct sdshdr8);
        case SDS_TYPE_16:
            return sizeof(struct sdshdr16);
        case SDS_TYPE_32:
            return sizeof(struct sdshdr32);
        case SDS_TYPE_64:
            return sizeof(struct sdshdr64);
    }
    return 0;
}

/**
 * 给定字符串长度 能够满足字符串长度需求的最好的sds类型
 * 每种sds类型所能表达的len长度上限为边界 进行查找最优类型
 * @param string_size 字符串长度
 * @return sds类型枚举值
 */
static inline char sdsReqType(size_t string_size) {
    if (string_size < 1<<5)
        return SDS_TYPE_5;
    if (string_size < 1<<8)
        return SDS_TYPE_8;
    if (string_size < 1<<16)
        return SDS_TYPE_16;
#if (LONG_MAX == LLONG_MAX)
    if (string_size < 1ll<<32)
        return SDS_TYPE_32;
    return SDS_TYPE_64;
#else
    return SDS_TYPE_32;
#endif
}

/**
 * 每种sds字符串类型能够表达的长度上限
 * @param type sds类型枚举值
 * @return sds字符串类型能够表达的长度上限 就是sdshdr中len成员类型的最大值
 */
static inline size_t sdsTypeMaxSize(char type) {
    if (type == SDS_TYPE_5)
        return (1<<5) - 1;
    if (type == SDS_TYPE_8)
        return (1<<8) - 1;
    if (type == SDS_TYPE_16)
        return (1<<16) - 1;
#if (LONG_MAX == LLONG_MAX)
    if (type == SDS_TYPE_32)
        return (1ll<<32) - 1;
#endif
    return -1; /* this is equivalent to the max SDS_TYPE_64 or SDS_TYPE_32 */
}

/* Create a new sds string with the content specified by the 'init' pointer
 * and 'initlen'.
 * If NULL is used for 'init' the string is initialized with zero bytes.
 * If SDS_NOINIT is used, the buffer is left uninitialized;
 *
 * The string is always null-termined (all the sds strings are, always) so
 * even if you create an sds string with:
 *
 * mystring = sdsnewlen("abc",3);
 *
 * You can print the string with printf() as there is an implicit \0 at the
 * end of the string. However the string is binary safe and can contain
 * \0 characters in the middle, as the length is stored in the sds header. */

/**
 * 根据C字符串创建sds实例
 * @param init C字符串
 * @param initlen C字符串长度
 * @param trymalloc 标识符 控制选择的内存分配函数 不是很重要 无非就是上层还是下层去关注OOM的处理
 *                  非0 try一下 不用关注OOM的处理策略
 *                  0 不try 需要关注OOM的处理策略
 * @return sds字符串实例 指针指向的实际上是sds的buf数组
 */
sds _sdsnewlen(const void *init, size_t initlen, int trymalloc) {
    void *sh;
    sds s;
	// 根据C字符串长度需求选择sds字符串类型
    char type = sdsReqType(initlen);
    /* Empty strings are usually created in order to append. Use type 8
     * since type 5 is not good at this. */
	/**
	 * 注释是说的场景问题 创建空字符串的sds字符串大多数可能都是为了用来做字符串拼接使用的
	 * 应该是考虑到2点
	 * <ul>
	 *   <li>sds5字符串没有alloc 也就是sds5没有表达空间预分配的能力</li>
	 *   <li>sds5字符串长度较短</li>
	 * </ul>
	 * 因此这种场景就直接sds8起步
	 */
    if (type == SDS_TYPE_5 && initlen == 0) type = SDS_TYPE_8;
	// sizeof计算出不同sds类型的header大小 为了从buf指针移动
    int hdrlen = sdsHdrSize(type);
    unsigned char *fp; /* flags pointer. */
	// 向OS内存分配器申请内存 分配的实际的内存空间大小
    size_t usable;

	/**
	 * 经典溢出检测
	 * 要表达一个initlen长度的字符串 sds的空间大小至少是包含头+字符串长度+\0结束标识符
	 * 这样的内存大小是要向OS内存分配器申请的 其接收的参数类型是sizet
	 * 3个整数求和结果肯定要做溢出检测的
	 */
    assert(initlen + hdrlen + 1 > initlen); /* Catch size_t overflow */
	// OS内存分配器分配的内存
    sh = trymalloc?
        s_trymalloc_usable(hdrlen+initlen+1, &usable) :
        s_malloc_usable(hdrlen+initlen+1, &usable);
    if (sh == NULL) return NULL;
    if (init==SDS_NOINIT)
        init = NULL;
    else if (!init)
        memset(sh, 0, hdrlen+initlen+1); // 没有指定sds初始化方法 将sds内存全部置0
	// s指向buf数组
    s = (char*)sh+hdrlen;
	// fp指向flags
    fp = ((unsigned char*)s)-1;
	// buf数组剩余可用空间
    usable = usable-hdrlen-1;
	/**
	 * 为什么要做下面这一步的限制呢
	 * 因为usable是要赋值给alloc的
	 * 5种sds类型种sds5是没有维护alloc的 可以抛开不谈
	 * 其他4种sds类型 每种alloc的类型跟len的类型都是一样的
	 * 因此下面的判断是防止数据溢出
	 * 比如sds8中len和alloc的数据类型都是8bit 能存放的最大的值也就是2^8-1
	 */
    if (usable > sdsTypeMaxSize(type))
        usable = sdsTypeMaxSize(type);
	/**
	 * 对sds数据结构len alloc flags成员进行赋值操作
	 * sds5特殊之外其他都是直接赋值
	 * sds5将len和类型信息都编码近flags成员中 高5位编码字符串长度 低3位编码sds类型
	 */
    switch(type) {
        case SDS_TYPE_5: {
            *fp = type | (initlen << SDS_TYPE_BITS); // flags
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            sh->len = initlen;
            sh->alloc = usable;
            *fp = type;
            break;
        }
    }
	// C字符串内容拷贝到sds字符串中
    if (initlen && init)
        memcpy(s, init, initlen);
	/**
	 * 保留C字符串结束符的原因是sds字符串返回给用户的是指向的buf数组
	 * 为了完全兼容C字符串的API 就保留了结束符
	 */
    s[initlen] = '\0';
    return s;
}

/**
 * 根据C字符串创建sds字符串
 * @param init C字符串
 * @param initlen C字符串长度
 * @return sds字符串实例 指向的sds的buf数组
 */
sds sdsnewlen(const void *init, size_t initlen) {
    return _sdsnewlen(init, initlen, 0);
}

/**
 * 根据C字符串创建sds字符串
 * @param init C字符串
 * @param initlen C字符串长度
 * @return sds字符串实例 指向的sds的buf数组
 */
sds sdstrynewlen(const void *init, size_t initlen) {
    return _sdsnewlen(init, initlen, 1);
}

/* Create an empty (zero length) sds string. Even in this case the string
 * always has an implicit null term. */
/**
 * 创建空字符串
 * 创建的是sds8类型的字符串
 * @return sds8类型的字符串实例
 */
sds sdsempty(void) {
    return sdsnewlen("",0);
}

/* Create a new sds string starting from a null terminated C string. */
/**
 * 根据C字符串创建sds字符串
 * @param init C字符串
 * @return sds字符串实例 指向的sds的buf数组
 */
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/* Duplicate an sds string. */
/**
 * sds字符串拷贝
 * @param s 源sds字符串 因为sds实例的指针实际指向的是buf数组 因此完全兼容C字符串 直接调用根据C字符串创建sds字符串的API就行
 * @return sds实例
 */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/* Free an sds string. No operation is performed if 's' is NULL. */
/**
 * 释放sds内存
 * @param s 要释放内存的sds实例
 */
void sdsfree(sds s) {
    if (s == NULL) return;
	/**
	 * s指向的是buf数组
	 * 先根据buf数组低地址方向的flags计算出sds的head大小
	 * 再将s从buf数组位置向低地址方向移动到sds
	 * 向OS内存分配器申请释放内存
	 */
    s_free((char*)s-sdsHdrSize(s[-1]));
}

/* Set the sds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the sds string is hacked manually in some
 * way, like in the following example:
 *
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
/**
 * 根据C字符串结束符更新sds字符串长度
 * 可能是sds字符串创建好后 用户手动重置了buf数组中的C字符串结束符
 * 因此要该API同步对应字符串的长度
 * @param s sds字符串
 */
void sdsupdatelen(sds s) {
    size_t reallen = strlen(s);
    sdssetlen(s, reallen);
}

/* Modify an sds string in-place to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
/**
 * 字符串清空
 * 并不需要真正的操作地址空间 只需要修改标识就行 因为sds完全兼容C字符串
 * 所以要修改2个地方
 * <ul>
 *   <li>对于sds字符串而言 修改sds字符串head中的len成员值</li>
 *   <li>对于C字符串 重置字符串结束符</li>
 * </ul>
 * @param s sds字符串实例
 */
void sdsclear(sds s) {
    // 重置sds的字符串head中的len成员为0
    sdssetlen(s, 0);
	// 重置C字符串的结束符位置
    s[0] = '\0';
}

/* Enlarge the free space at the end of the sds string so that the caller
 * is sure that after calling this function can overwrite up to addlen
 * bytes after the end of the string, plus one more byte for nul term.
 *
 * Note: this does not change the *length* of the sds string as returned
 * by sdslen(), but only the free buffer space we have. */
/**
 * 扩展sds的可用内存 保证sds字符串buf预分配的可用内存至少得有addlen个byte
 * @param s sds实例
 * @param addlen sds字符串buf可用的边界
 * @return sds实例 可能还是之前字符串 也可能是扩容过buf内存的那个字符串 更甚就是个新的sds字符串
 */
sds sdsMakeRoomFor(sds s, size_t addlen) {
    void *sh, *newsh;
	// sds的buf中还剩的可用空间
    size_t avail = sdsavail(s);
    size_t len, newlen, reqlen;
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen;
    size_t usable;

    /* Return ASAP if there is enough space left. */
    if (avail >= addlen) return s;

	// 字符串长度
    len = sdslen(s);
	// 指向sds数据结构
    sh = (char*)s-sdsHdrSize(oldtype);
    reqlen = newlen = (len+addlen);
	// 典型溢出判断方式
    assert(newlen > len);   /* Catch size_t overflow */
	// 阈值限制防止内存的过度分配
    if (newlen < SDS_MAX_PREALLOC)
        newlen *= 2;
    else
        newlen += SDS_MAX_PREALLOC;

	// 满足新长度的sds类型
    type = sdsReqType(newlen);

    /* Don't use type 5: the user is appending to the string and type 5 is
     * not able to remember empty space, so sdsMakeRoomFor() must be called
     * at every appending operation. */
	/**
	 * sds5中没有alloc成员记录OS内存分配器实际分配的内存大小 自然不知道剩余可用空间
	 * 所以即使此时选用sds5类型 后面扩张还要选用被的sds类型
	 * 与其又可能发生2次sds实例创建 不如争取1次sds实例创建的开销成本
	 * <ul>
	 *   <li>比如此时用sds5 下一次势必sds5不满足使用需求 要创建一个比如sds8 那么总共发生了2次的字符串创建</li>
	 *   <li>比如此时用sds8 下一次用alloc-len计算发现还有可用空间 并且也满足需求 或者发现需求的大小也在sds8的满足范围之内 那么总共发生了1次的字符串创建</li>
	 * </ul>
	 */
    if (type == SDS_TYPE_5) type = SDS_TYPE_8;

    hdrlen = sdsHdrSize(type);
    assert(hdrlen + newlen + 1 > reqlen);  /* Catch size_t overflow */
    if (oldtype==type) {
		/**
	     * 当前sds字符串类型足够表达需要的新长度
	     * 也就是head布局可以复用
	     * 只要扩容buf数组即可
	     * 并且记下来OS内存分配器实际分配的大小 要用来更新alloc的值
		 */
        newsh = s_realloc_usable(sh, hdrlen+newlen+1, &usable);
        if (newsh == NULL) return NULL;
        s = (char*)newsh+hdrlen;
    } else {
        /* Since the header size changes, need to move the string forward,
         * and can't use realloc */
		/**
		 *
		 */
        newsh = s_malloc_usable(hdrlen+newlen+1, &usable);
        if (newsh == NULL) return NULL;
        memcpy((char*)newsh+hdrlen, s, len+1);
        s_free(sh);
        s = (char*)newsh+hdrlen;
		/**
		 * 因为header升级了 所以要对新的header中成员进行赋值
		 * <ul>
		 *   <li>len</li>
		 *   <li>alloc</li>
		 *   <li>flags</li>
		 * </ul>
		 */
        s[-1] = type;
        sdssetlen(s, len);
    }
	// 总共从内存分配器哪儿拿来的内存刨去header的占用和buf数组中C字符串结束符的占用就是分配下来buf总共可用的
    usable = usable-hdrlen-1;
	// 每种sds数据结构的len和alloc的类型都是一样的 校验一下防止溢出
    if (usable > sdsTypeMaxSize(type))
        usable = sdsTypeMaxSize(type);
	// 赋值header中的alloc成员
    sdssetalloc(s, usable);
    return s;
}

/* Reallocate the sds string so that it has no free space at the end. The
 * contained string remains not altered, but next concatenation operations
 * will require a reallocation.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * sds动态内存的体现 释放sds中未使用的空间 达到节省内存的效果
 * @param s sds实例
 * @return 释放内存空间之后的sds实例 可能还是之前的sds字符串 也可能还是跟之前同类型的sds字符串 也可又能是一个降级类型的字符串
 */
sds sdsRemoveFreeSpace(sds s) {
    void *sh, *newsh;
	// 实际使用的sds类型
    char type, oldtype = s[-1] & SDS_TYPE_MASK;
    int hdrlen, oldhdrlen = sdsHdrSize(oldtype);
	// 字符串长度
    size_t len = sdslen(s);
	// sds字符串buf的可用空间
    size_t avail = sdsavail(s);
    sh = (char*)s-oldhdrlen;

    /* Return ASAP if there is no space left. */
	// buf中已经没有可用内存了 减无可减的情况
    if (avail == 0) return s;

    /* Check what would be the minimum SDS header that is just good enough to
     * fit this string. */
	// 满足字符串长度需求的期望的sds类型
    type = sdsReqType(len);
    hdrlen = sdsHdrSize(type);

    /* If the type is the same, or at least a large enough type is still
     * required, we just realloc(), letting the allocator to do the copy
     * only if really needed. Otherwise if the change is huge, we manually
     * reallocate the string to use the different header type. */
    if (oldtype==type || type > SDS_TYPE_8) {
	    /**
	     * sds类型不换 header中的内容可以复用 根据需要的字符串长度重新分配内存
	     * header+字符串+字符串结束符
	     */
        newsh = s_realloc(sh, oldhdrlen+len+1);
        if (newsh == NULL) return NULL;
        s = (char*)newsh+oldhdrlen;
    } else {
		/**
	     * sds类型降级了
	     * 新的header+字符串+字符串结束符
		 */
        newsh = s_malloc(hdrlen+len+1);
        if (newsh == NULL) return NULL;
		// 字符串可以直接拷贝 header的len和flags成员手动赋值
        memcpy((char*)newsh+hdrlen, s, len+1);
		// 释放老的sds内存
        s_free(sh);
		// 指向新的sds的C字符串
        s = (char*)newsh+hdrlen;
		// 新的sds的flags赋值
        s[-1] = type;
        sdssetlen(s, len);
    }
	// header中alloc成员赋值
    sdssetalloc(s, len);
    return s;
}

/* Return the total size of the allocation of the specified sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
/**
 * sds占用的空间 包括
 * <ul>
 *   <li>header</li>
 *   <li>buf缓冲区 可用空间 就是alloc标识的大小</li>
 *   <li>buf缓冲区 C字符串结束符标识符占位</li>
 * </ul>
 * @param s sds字符串
 * @return sds字符串占用的空间
 */
size_t sdsAllocSize(sds s) {
    size_t alloc = sdsalloc(s);
    return sdsHdrSize(s[-1])+alloc+1;
}

/* Return the pointer of the actual SDS allocation (normally SDS strings
 * are referenced by the start of the string buffer). */
/**
 * sds的指针暴露给用户的指向buf数组 移动到header上
 * @param s sds实例
 * @return 指向sds的header的指针
 */
void *sdsAllocPtr(sds s) {
    return (void*) (s-sdsHdrSize(s[-1]));
}

/* Increment the sds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * This function is used in order to fix the string length after the
 * user calls sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * Usage example:
 *
 * Using sdsIncrLen() and sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * sds string without copying into an intermediate buffer:
 *
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * sdsIncrLen(s, nread);
 */
/**
 * sds字符串长度增加
 * 这个增加字符串长度是利用buf缓冲区预分配的空间 不额外向OS系统申请内存
 * 如何增加字符串长度
 * <ul>
 *   <li>必要的校验 buf剩余可用的内存空间够不够字符串长度增加使用</li>
 *   <li>更新sdshdr中len成员</li>
 *   <li>更新buf中C字符串结束符位置</li>
 * </ul>
 * @param s sds实例
 * @param incr 字符串要增加的长度
 */
void sdsIncrLen(sds s, ssize_t incr) {
    // 暴露给用户的sds指针指向的buf数组 向低地址移动找到flags成员
    unsigned char flags = s[-1];
    size_t len;
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
		    // flags
            unsigned char *fp = ((unsigned char*)s)-1;
			// flags的高5bit读出来sds字符串长度
            unsigned char oldlen = SDS_TYPE_5_LEN(flags);
			// 必要的校验
            assert((incr > 0 && oldlen+incr < 32) || (incr < 0 && oldlen >= (unsigned int)(-incr)));
            *fp = SDS_TYPE_5 | ((oldlen+incr) << SDS_TYPE_BITS);
            len = oldlen+incr;
            break;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            assert((incr >= 0 && sh->alloc-sh->len >= incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (unsigned int)incr) || (incr < 0 && sh->len >= (unsigned int)(-incr)));
            len = (sh->len += incr);
            break;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            assert((incr >= 0 && sh->alloc-sh->len >= (uint64_t)incr) || (incr < 0 && sh->len >= (uint64_t)(-incr)));
            len = (sh->len += incr);
            break;
        }
        default: len = 0; /* Just to avoid compilation warnings. */
    }
	// 更新sds的buf中C字符串的结束符位置
    s[len] = '\0';
}

/* Grow the sds to have the specified length. Bytes that were not part of
 * the original length of the sds will be set to zero.
 *
 * if the specified length is smaller than the current length, no operation
 * is performed. */
/**
 * sds字符串长度至少为len
 * @param s sds字符串
 * @param len 要sds字符串长度满足的要求
 * @return sds字符串
 */
sds sdsgrowzero(sds s, size_t len) {
    // 当前sds长度
    size_t curlen = sdslen(s);
	// sds的长度已经满足了要求 不需要任何操作
    if (len <= curlen) return s;
	// sds字符串长度不够 扩容
    s = sdsMakeRoomFor(s,len-curlen);
    if (s == NULL) return NULL;

    /* Make sure added region doesn't contain garbage */
	// 扩容出来的空间置0
    memset(s+curlen,0,(len-curlen+1)); /* also set trailing \0 byte */
	// sds新的长度
    sdssetlen(s, len);
    return s;
}

/* Append the specified binary-safe string pointed by 't' of 'len' bytes to the
 * end of the specified sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * 将C字符串指定长度拼接到sds字符串上 追加拷贝
 * @param s sds字符串 C字符串拼接到的宿主sds字符串
 * @param t C字符串
 * @param len 将C字符串中多长拷贝到sds中
 * @return sds字符串
 */
sds sdscatlen(sds s, const void *t, size_t len) {
    size_t curlen = sdslen(s);

    s = sdsMakeRoomFor(s,len);
    if (s == NULL) return NULL;
	// 将C字符串指定长度字符拷贝到sds已有的C字符串后面 也就是从已有的\0处开始追加
    memcpy(s+curlen, t, len);
	// sds新的长度
    sdssetlen(s, curlen+len);
	// sds新的C字符串结束符
    s[curlen+len] = '\0';
    return s;
}

/* Append the specified null terminated C string to the sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * 将C字符串指定长度拼接到sds字符串上 追加拷贝
 * @param s sds字符串
 * @param t C字符串 把整个C字符串追加到sds上
 * @return sds字符串
 */
sds sdscat(sds s, const char *t) {
    return sdscatlen(s, t, strlen(t));
}

/* Append the specified sds 't' to the existing sds 's'.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
/**
 * sds字符串追加到sds字符串上
 * @param s sds字符串
 * @param t 要把哪个sds字符串追加到别的sds字符串上
 * @return sds字符串
 */
sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

/* Destructively modify the sds string 's' to hold the specified binary
 * safe string pointed by 't' of length 'len' bytes. */
/**
 * 将C字符串中指定长度的子串拷贝到sds中 不是追加
 * @param s sds实例
 * @param t C字符串
 * @param len 要拷贝的C字符串长度
 * @return sds实例
 */
sds sdscpylen(sds s, const char *t, size_t len) {
    // 校验buf缓冲区大小够不够
    if (sdsalloc(s) < len) {
        s = sdsMakeRoomFor(s,len-sdslen(s));
        if (s == NULL) return NULL;
    }
	// C字符串拷贝到buf数组中
    memcpy(s, t, len);
	// 字符串结束符
    s[len] = '\0';
	// 赋值sds的len成员
    sdssetlen(s, len);
    return s;
}

/* Like sdscpylen() but 't' must be a null-termined string so that the length
 * of the string is obtained with strlen(). */
/**
 * 将C字符串拷贝到sds字符串中 不是追加
 * @param s sds实例 C字符串拷贝到哪儿
 * @param t C字符串
 * @return sds字符串
 */
sds sdscpy(sds s, const char *t) {
    return sdscpylen(s, t, strlen(t));
}

/* Helper for sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least
 * SDS_LLSTR_SIZE bytes.
 *
 * The function returns the length of the null-terminated string
 * representation stored at 's'. */
#define SDS_LLSTR_SIZE 21
/**
 * 数字转字符串
 * <ul>
 *   <li>数字部分用取模方式依次从低位把数字取出来放到字符串上</li>
 *   <li>最后根据数字正负加上符号</li>
 *   <li>再把刚才追加的内容反转</li>
 * </ul>
 * 比如-1234
 * <ul>
 *   <li>从4到1依次取出数字转字符 形成字符串4321</li>
 *   <li>负数要加上符号 字符串变成4321-</li>
 *   <li>对形成的内容反转 变成 -1234</li>
 * </ul>
 * @param s 数字转字符串往哪个字符串上追加
 * @param value 数字
 * @return 数字转成字符串的长度
 */
int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * a reversed string. */
	// 刨除数字的符号 先处理数字部分
    v = (value < 0) ? -value : value;
    p = s;
	// 把数字从低位逐次取出来专程字符追加到字符串s上
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
	// 数字是负数 字符串s上追加负号
    if (value < 0) *p++ = '-';

    /* Compute length and add null term. */
	// 记录数字的长度 如果数字是负数就包含上负号 将来返回给调用方
    l = p-s;
	// C字符串描述符
    *p = '\0';

    /* Reverse the string. */
    p--;
	// 将数字(负数的话包括负号)整个反转一下
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
	// 告知调用方数字有多长
    return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
/**
 * 把无符号数字转换成字符串
 * 跟上面有符号数转字符串逻辑一样 仅仅是不用处理负号了
 * @param s 字符串
 * @param v 数字
 * @return 数字多长
 */
int sdsull2str(char *s, unsigned long long v) {
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * a reversed string. */
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Create an sds string from a long long value. It is much faster than:
 *
 * sdscatprintf(sdsempty(),"%lld\n", value);
 */
/**
 * 根据数字创建sds字符串
 * @param value 数字
 * @return sds字符串
 */
sds sdsfromlonglong(long long value) {
    char buf[SDS_LLSTR_SIZE];
	// 数字先转成C字符串 记下字符串长度
    int len = sdsll2str(buf,value);
	// 用C字符串创建sds字符串
    return sdsnewlen(buf,len);
}

/* Like sdscatprintf() but gets va_list instead of being variadic. */
/**
 * 格式化输出
 * @param s 格式化的结果拼接到这个sds字符串上
 * @param fmt 格式化
 * @param ap 格式化的参数列表
 * @return 拼接完的sds字符串
 */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;
    int bufstrlen;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = s_malloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Alloc enough space for buffer and \0 after failing to
     * fit the string in the current buffer size. */
	/**
	 * 参数列表的拷贝用的C标准库函数
	 * va_start函数和va_end函数必须成对使用
	 */
    while(1) {
	    // 参数列表的拷贝 把ap拷贝到cpy
        va_copy(cpy,ap);
		// sds字符串的格式化输出就是调用的C字符串的C标准库
        bufstrlen = vsnprintf(buf, buflen, fmt, cpy);
		// 显式调用va_end 保证成对使用
        va_end(cpy);
        if (bufstrlen < 0) {
            if (buf != staticbuf) s_free(buf);
            return NULL;
        }
        if (((size_t)bufstrlen) >= buflen) {
            if (buf != staticbuf) s_free(buf);
            buflen = ((size_t)bufstrlen) + 1;
            buf = s_malloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
	// 用格式化输出的C字符串结果拼接到s这个sds字符串上
    t = sdscatlen(s, buf, bufstrlen);
    if (buf != staticbuf) s_free(buf);
    return t;
}

/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
/**
 * 格式化输出的结果拼接到sds字符串
 * @param s 格式化输出的结果拼接到哪个sds字符串上
 * @param fmt 格式化字符串
 * @param ... 格式化字符串的参数列表
 * @return 拼接了格式化字符串的结果
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
	// va_start跟va_end成对使用
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
/**
 * 解析格式化字符串 拼接到s字符串
 * @param s 格式化输出的结果拼接到哪个sds字符串上
 * @param fmt 格式化字符串 C字符串
 * @param ... 格式化字符串的参数列表
 * @return 拼接了格式化字符串的结果
 */
sds sdscatfmt(sds s, char const *fmt, ...) {
    // 字符串长度
    size_t initlen = sdslen(s);
    const char *f = fmt;
    long i;
    va_list ap;

    /* To avoid continuous reallocations, let's start with a buffer that
     * can hold at least two times the format string itself. It's not the
     * best heuristic but seems to work in practice. */
	// 乘以2没什么理由 仅仅是一个人为经验值 争取减少或者避免后面的扩容
    s = sdsMakeRoomFor(s, strlen(fmt)*2);
    va_start(ap,fmt);
    f = fmt;    /* Next format specifier byte to process. */
    i = initlen; /* Position of the next byte to write to dest str. */
    while(*f) {
        char next, *str;
        size_t l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (sdsavail(s)==0) {
            s = sdsMakeRoomFor(s,1);
        }

        switch(*f) {
        case '%':
            next = *(f+1); // %后面跟的是什么
            f++;
            switch(next) {
            case 's': // c的str
            case 'S': // redis的sds
                str = va_arg(ap,char*);
                l = (next == 's') ? strlen(str) : sdslen(str);
                if (sdsavail(s) < l) {
                    s = sdsMakeRoomFor(s,l);
                }
                memcpy(s+i,str,l);
                sdsinclen(s,l);
                i += l;
                break;
            case 'i': // signed int
            case 'I': // 64 int
                if (next == 'i')
                    num = va_arg(ap,int);
                else
                    num = va_arg(ap,long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsll2str(buf,num);
                    if (sdsavail(s) < l) {
                        s = sdsMakeRoomFor(s,l);
                    }
                    memcpy(s+i,buf,l);
                    sdsinclen(s,l);
                    i += l;
                }
                break;
            case 'u': // unsigned int
            case 'U': // 64位的unsigned int
                if (next == 'u')
                    unum = va_arg(ap,unsigned int);
                else
                    unum = va_arg(ap,unsigned long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsull2str(buf,unum);
                    if (sdsavail(s) < l) {
                        s = sdsMakeRoomFor(s,l);
                    }
                    memcpy(s+i,buf,l);
                    sdsinclen(s,l);
                    i += l;
                }
                break;
            default: /* Handle %% and generally %<unknown>. */
                s[i++] = next;
                sdsinclen(s,1);
                break;
            }
            break;
        default:
            s[i++] = *f;
            sdsinclen(s,1);
            break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;
}

/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminted C string.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "HelloWorld".
 */
/**
 * 清掉sds字符串两头在cset中的内容
 * @param s sds字符串
 * @param cset 模式
 * @return 清除之后的sds字符串
 */
sds sdstrim(sds s, const char *cset) {
    char *start, *end, *sp, *ep;
    size_t len;

    sp = start = s;
    ep = end = s+sdslen(s)-1;
	// 找到sds字符串左边第一个不在cset中的位置sp
    while(sp <= end && strchr(cset, *sp)) sp++;
	// 找到sds字符串右边第一个不在cset中的位置ep
    while(ep > sp && strchr(cset, *ep)) ep--;
	// 此时[sp...ep]就是结果字符串 要对边界进行检查
    len = (sp > ep) ? 0 : ((ep-sp)+1);
	// 移动字符位置
    if (s != sp) memmove(s, sp, len);
	// C字符串结束符
    s[len] = '\0';
	// 新字符串的长度
    sdssetlen(s,len);
    return s;
}

/* Changes the input string to be a subset of the original.
 * It does not release the free space in the string, so a call to
 * sdsRemoveFreeSpace may be wise after. */
/**
 * 子串
 * @param s 源sds字符串
 * @param start 开始位置
 * @param len 子串长度
 */
void sdssubstr(sds s, size_t start, size_t len) {
    /* Clamp out of range input */
    size_t oldlen = sdslen(s);
	// 参数校验 开始位置超出了源字符串边界 判定子串结果是空字符串
    if (start >= oldlen) start = len = 0;
	// 子串长度校验
    if (len > oldlen-start) len = oldlen-start;

    /* Move the data */
	// 将子串移动到字符数组的顶端
    if (len) memmove(s, s+start, len);
	// 重置sds中C字符串结束符位置
    s[len] = 0;
	// 重置sds字符串长度
    sdssetlen(s,len);
}

/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * NOTE: this function can be misleading and can have unexpected behaviour,
 * specifically when you want the length of the new string to be 0.
 * Having start==end will result in a string with one character.
 * please consider using sdssubstr instead.
 *
 * Example:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
/**
 * 截串
 * @param s 要截断哪个字符串 [start...end]左右都是闭区间
 * @param start 开始角标 负1表示最后一个位置 以此类推
 * @param end 开始角标 负1表示最后一个位置 以此类推
 */
void sdsrange(sds s, ssize_t start, ssize_t end) {
    size_t newlen, len = sdslen(s);
    if (len == 0) return;
    if (start < 0)
        start = len + start;
    if (end < 0)
        end = len + end;
    newlen = (start > end) ? 0 : (end-start)+1;
    sdssubstr(s, start, newlen);
}

/* Apply tolower() to every character of the sds string 's'. */
/**
 * 字符串转小写
 * @param s sds字符串
 */
void sdstolower(sds s) {
    // 字符串长度
    size_t len = sdslen(s), j;
	// 轮询C字符串每个字符转小写
    for (j = 0; j < len; j++) s[j] = tolower(s[j]);
}

/* Apply toupper() to every character of the sds string 's'. */
/**
 * 字符串转大写
 * @param s sds字符串
 */
void sdstoupper(sds s) {
    // 字符串长度
    size_t len = sdslen(s), j;
	// 轮询C字符串每个字符转大写
    for (j = 0; j < len; j++) s[j] = toupper(s[j]);
}

/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
/**
 * 字典序比较
 * @return 0 二者相等
 *         -1 s1<s2
 *         1 s1>s2
 */
int sdscmp(const sds s1, const sds s2) {
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;
    cmp = memcmp(s1,s2,minlen);
    if (cmp == 0) return l1>l2? 1: (l1<l2? -1: 0);
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
 /**
  * 给定分隔符进行字符串分割 分割的子串创建sds实例放到数组中
  * @param s 要分割的字符串
  * @param len 待分割的字符串长度
  * @param sep 分隔符
  * @param seplen 分隔符长度
  * @param count 源字符串被分割成了几个子串
  * @return 分割好的子串放到数组中
  */
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count) {
    int elements = 0, slots = 5;
	// start缓存着待收集的子串开始脚标 [start...j-1] [j...j+seplen-1]遇到了j开始的位置是分隔符 就收集start到j-1的子串
    long start = 0, j;
    sds *tokens;

    if (seplen < 1 || len < 0) return NULL;
    // sds数组内存申请 预申请长度为5的数组
    tokens = s_malloc(sizeof(sds)*slots);
    if (tokens == NULL) return NULL;

    if (len == 0) { // 源字符串为空 没法切割
        *count = 0;
        return tokens;
    }
	// 遍历字符串 [j...]可能是需要的子串
    for (j = 0; j < (len-(seplen-1)); j++) {
        /* make sure there is room for the next element and the final one */
        if (slots < elements+2) { // sds数组扩容
            sds *newtokens;

            slots *= 2;
            newtokens = s_realloc(tokens,sizeof(sds)*slots);
            if (newtokens == NULL) goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
		// 一旦发现[j...j+spelen-1]是分隔符内容 就收集j之前的内容
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {
            tokens[elements] = sdsnewlen(s+start,j-start);
            if (tokens[elements] == NULL) goto cleanup;
            elements++;
            start = j+seplen;
            j = j+seplen-1; /* skip the separator */
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) goto cleanup;
    elements++;
    *count = elements;
    return tokens;

cleanup:
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);
        s_free(tokens);
        *count = 0;
        return NULL;
    }
}

/* Free the result returned by sdssplitlen(), or do nothing if 'tokens' is NULL. */
void sdsfreesplitres(sds *tokens, int count) {
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]);
    s_free(tokens);
}

/* Append to the sds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatrepr(sds s, const char *p, size_t len) {
    s = sdscatlen(s,"\"",1);
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': s = sdscatlen(s,"\\n",2); break;
        case '\r': s = sdscatlen(s,"\\r",2); break;
        case '\t': s = sdscatlen(s,"\\t",2); break;
        case '\a': s = sdscatlen(s,"\\a",2); break;
        case '\b': s = sdscatlen(s,"\\b",2); break;
        default:
            if (isprint(*p))
                s = sdscatprintf(s,"%c",*p);
            else
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
    return sdscatlen(s,"\"",1);
}

/* Helper function for sdssplitargs() that returns non zero if 'c'
 * is a valid hex digit. */
int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* Helper function for sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
int hex_digit_to_int(char c) {
    switch(c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 */
sds *sdssplitargs(const char *line, int *argc) {
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        /* skip blanks */
        while(*p && isspace(*p)) p++;
        if (*p) {
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                                             is_hex_digit(*(p+2)) &&
                                             is_hex_digit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p+2))*16)+
                                hex_digit_to_int(*(p+3));
                        current = sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':
                        inq=1;
                        break;
                    case '\'':
                        insq=1;
                        break;
                    default:
                        current = sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            vector = s_realloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) vector = s_malloc(sizeof(void*));
            return vector;
        }
    }

err:
    while((*argc)--)
        sdsfree(vector[*argc]);
    s_free(vector);
    if (current) sdsfree(current);
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * The function returns the sds string pointer, that is always the same
 * as the input pointer since no resize is needed. */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {
    size_t j, i, l = sdslen(s);

    for (j = 0; j < l; j++) {
        for (i = 0; i < setlen; i++) {
            if (s[j] == from[i]) {
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an sds string. */
sds sdsjoin(char **argv, int argc, char *sep) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc-1) join = sdscat(join,sep);
    }
    return join;
}

/* Like sdsjoin, but joins an array of SDS strings. */
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen) {
    sds join = sdsempty();
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscatsds(join, argv[j]);
        if (j != argc-1) join = sdscatlen(join,sep,seplen);
    }
    return join;
}

/* Wrappers to the allocators used by SDS. Note that SDS will actually
 * just use the macros defined into sdsalloc.h in order to avoid to pay
 * the overhead of function calls. Here we define these wrappers only for
 * the programs SDS is linked to, if they want to touch the SDS internals
 * even if they use a different allocator. */
void *sds_malloc(size_t size) { return s_malloc(size); }
void *sds_realloc(void *ptr, size_t size) { return s_realloc(ptr,size); }
void sds_free(void *ptr) { s_free(ptr); }

/* Perform expansion of a template string and return the result as a newly
 * allocated sds.
 *
 * Template variables are specified using curly brackets, e.g. {variable}.
 * An opening bracket can be quoted by repeating it twice.
 */
sds sdstemplate(const char *template, sdstemplate_callback_t cb_func, void *cb_arg)
{
    sds res = sdsempty();
    const char *p = template;

    while (*p) {
        /* Find next variable, copy everything until there */
        const char *sv = strchr(p, '{');
        if (!sv) {
            /* Not found: copy till rest of template and stop */
            res = sdscat(res, p);
            break;
        } else if (sv > p) {
            /* Found: copy anything up to the begining of the variable */
            res = sdscatlen(res, p, sv - p);
        }

        /* Skip into variable name, handle premature end or quoting */
        sv++;
        if (!*sv) goto error;       /* Premature end of template */
        if (*sv == '{') {
            /* Quoted '{' */
            p = sv + 1;
            res = sdscat(res, "{");
            continue;
        }

        /* Find end of variable name, handle premature end of template */
        const char *ev = strchr(sv, '}');
        if (!ev) goto error;

        /* Pass variable name to callback and obtain value. If callback failed,
         * abort. */
        sds varname = sdsnewlen(sv, ev - sv);
        sds value = cb_func(varname, cb_arg);
        sdsfree(varname);
        if (!value) goto error;

        /* Append value to result and continue */
        res = sdscat(res, value);
        sdsfree(value);
        p = ev + 1;
    }

    return res;

error:
    sdsfree(res);
    return NULL;
}

#ifdef REDIS_TEST
#include <stdio.h>
#include <limits.h>
#include "testhelp.h"

#define UNUSED(x) (void)(x)

static sds sdsTestTemplateCallback(sds varname, void *arg) {
    UNUSED(arg);
    static const char *_var1 = "variable1";
    static const char *_var2 = "variable2";

    if (!strcmp(varname, _var1)) return sdsnew("value1");
    else if (!strcmp(varname, _var2)) return sdsnew("value2");
    else return NULL;
}

int sdsTest(int argc, char **argv, int accurate) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(accurate);

    {
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
            sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0);

        sdsfree(x);
        x = sdsnewlen("foo",2);
        test_cond("Create a string with specified length",
            sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0);

        x = sdscat(x,"bar");
        test_cond("Strings concatenation",
            sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        test_cond("sdscpy() against an originally longer string",
            sdslen(x) == 1 && memcmp(x,"a\0",2) == 0);

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
            sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        test_cond("sdscatprintf() seems working in the base case",
            sdslen(x) == 3 && memcmp(x,"123\0",4) == 0);

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"a%cb",0);
        test_cond("sdscatprintf() seems working with \\0 inside of result",
            sdslen(x) == 3 && memcmp(x,"a\0""b\0",4) == 0);

        {
            sdsfree(x);
            char etalon[1024*1024];
            for (size_t i = 0; i < sizeof(etalon); i++) {
                etalon[i] = '0';
            }
            x = sdscatprintf(sdsempty(),"%0*d",(int)sizeof(etalon),0);
            test_cond("sdscatprintf() can print 1MB",
                sdslen(x) == sizeof(etalon) && memcmp(x,etalon,sizeof(etalon)) == 0);
        }

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        test_cond("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0);
        printf("[%s]\n",x);

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," x");
        test_cond("sdstrim() works when all chars match",
            sdslen(x) == 0);

        sdsfree(x);
        x = sdsnew(" x ");
        sdstrim(x," ");
        test_cond("sdstrim() works when a single char remains",
            sdslen(x) == 1 && x[0] == 'x');

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0);

        y = sdsdup(x);
        sdsrange(y,1,1);
        test_cond("sdsrange(...,1,1)",
            sdslen(y) == 1 && memcmp(y,"i\0",2) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        test_cond("sdsrange(...,1,-1)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        test_cond("sdsrange(...,-2,-1)",
            sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        test_cond("sdsrange(...,2,1)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        test_cond("sdsrange(...,1,100)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        test_cond("sdsrange(...,100,100)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,4,6);
        test_cond("sdsrange(...,4,6)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0);

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,3,6);
        test_cond("sdsrange(...,3,6)",
            sdslen(y) == 1 && memcmp(y,"o\0",2) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x,y) > 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) == 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) < 0);

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        test_cond("sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0);

        {
            unsigned int oldfree;
            char *p;
            int i;
            size_t step = 10, j;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            test_cond("sdsnew() free/len buffers", sdslen(x) == 1 && sdsavail(x) == 0);

            /* Run the test a few times in order to hit the first two
             * SDS header types. */
            for (i = 0; i < 10; i++) {
                size_t oldlen = sdslen(x);
                x = sdsMakeRoomFor(x,step);
                int type = x[-1]&SDS_TYPE_MASK;

                test_cond("sdsMakeRoomFor() len", sdslen(x) == oldlen);
                if (type != SDS_TYPE_5) {
                    test_cond("sdsMakeRoomFor() free", sdsavail(x) >= step);
                    oldfree = sdsavail(x);
                    UNUSED(oldfree);
                }
                p = x+oldlen;
                for (j = 0; j < step; j++) {
                    p[j] = 'A'+j;
                }
                sdsIncrLen(x,step);
            }
            test_cond("sdsMakeRoomFor() content",
                memcmp("0ABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJABCDEFGHIJ",x,101) == 0);
            test_cond("sdsMakeRoomFor() final length",sdslen(x)==101);

            sdsfree(x);
        }

        /* Simple template */
        x = sdstemplate("v1={variable1} v2={variable2}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() normal flow",
                  memcmp(x,"v1=value1 v2=value2",19) == 0);
        sdsfree(x);

        /* Template with callback error */
        x = sdstemplate("v1={variable1} v3={doesnotexist}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with callback error", x == NULL);

        /* Template with empty var name */
        x = sdstemplate("v1={", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with empty var name", x == NULL);

        /* Template with truncated var name */
        x = sdstemplate("v1={start", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with truncated var name", x == NULL);

        /* Template with quoting */
        x = sdstemplate("v1={{{variable1}} {{} v2={variable2}", sdsTestTemplateCallback, NULL);
        test_cond("sdstemplate() with quoting",
                  memcmp(x,"v1={value1} {} v2=value2",24) == 0);
        sdsfree(x);
    }
    test_report();
    return 0;
}
#endif
