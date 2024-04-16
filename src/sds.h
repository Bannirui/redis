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

#ifndef __SDS_H
#define __SDS_H

/**
 * sds字符串扩容预分配内存的阈值
 * 在该阈值之下可以成倍扩
 * 为了防止过度的内存分配
 * 限制了在阈值之上不要成倍扩内存
 */
#define SDS_MAX_PREALLOC (1024*1024)
extern const char *SDS_NOINIT;

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

/**
 * 字符数组
 * 为什么这样 我觉得是向下兼容C语言中的字符串类型
 */
typedef char *sds;

/* Note: sdshdr5 is never used, we just access the flags byte directly.
 * However is here to document the layout of type 5 SDS strings. */

/**
 * __attribute__ ((__packed__)) 告诉编译器取消结构体在编译过程中的对齐优化 按照实际字节占用进行对齐
 * 为什么禁止编译器优化呢
 * 因为5种类型的sds的结构是精心设计的
 * <ul>
 *   <li>1 首先为了能完全兼容C语言中的字符串 最终暴露给用户的依然是指向实际字符串数组的指针</li>
 *   <li>2 为了弥补C语言字符串的缺陷(比如二进制不安全 获取字符串长度时间复杂度太高 增减字符串长度对内存空间不优化) 设计了sds的头</li>
 *   <li>3 redis为了极致压缩内存 设计了5种类型的sds 也即有5种头类型 头里面存储的就是sds字符串的元数据信息 拿到了头信息就完全掌握了sds字符串信息</li>
 *   <li>4 现在已经确定了sds字符串指针指向的存储实际字符串的字符数组 也就是说往高地址空间是字符串内容 想读头信息只能往低地址空间了 又因为有5种类型 所以sds指针往低地址方向的第一部分得存储标识信息区分sds类型</li>
 *   <li>5 一旦知道了当前是哪种sds类型 又因为数据结构的大小在编译期间就决定了 便能够知道当前sds的头大小 一旦知道头大小就能偏移指针直接到sds数据结构的头 又因为sds类型知道了 自然而然可以随便玩转指针了</li>
 * </ul>
 * 如上种种 核心就是2点
 * <ul>
 *   <li>sds类型枚举的字段设计</li>
 *   <li>sds的数据结构不能被破坏</li>
 * </ul>
 * 所以才告诉编译器不要因为优化而改变布局
 *
 * 顺着上面的思路
 * 下面定义的5种sds数据结构也就不难理解了
 * 首先抛开sdshdr5这种类型不谈
 * <ul>
 *   <li>
 *     <ul>buf成员
 *       <li>要兼容C字符串 那么就是依然要保持字符数组的结构 并且将来暴露给用户的指针就是指向buf的</li>
 *       <li>将来要拿着指向buf的指针反向操作所代表的sds类型 因此需要有能力把指针从buf位置移动到sds位置 所以将buf设计成柔性数组 在编译期就确定数据结构大小 方便计算数据结构大小 从而进行指针移动</li>
 *     </ul>
 *   </li>
 *   <li>
 *     <ul>len成员
 *       <li>C字符串求长度是个O(n)时间复杂度的消耗 因此将字符长长度信息放在sds头中 优化成O(1)的操作 典型用空间换时间</li>
 *       <li>这个len本身的类型就决定了能表达多大的数字 也就是说不同长度的字符串倒逼选择不同的len类型 不同呢的len类型得设计对应的sdshdr数据结构</li>
 *     </ul>
 *   </li>
 *   <li>
 *     <ul>alloc成员
 *       <li>C字符串每一次的增删都意味着要重新分配内存然后拷贝原字符串或者memset</li>
 *       <li>sds为了减少一些场景的内存分配开销 用alloc记录一次分配好的内存大小 也就是说buf整个大小由alloc来表达 字符串长度由len来表达 因为要兼容C的字符串因此buf实际使用的就是len+1</li>
 *     </ul>
 *   </li>
 *   <li>
 *     <ul>flags成员
 *       <li>上面几个成员都是功能需要 确定好之后自然而然的问题就是如何确定不同sds类型</li>
 *       <li>并且因为sds指针指向的buf 所以flags有且只能放在buf的低地址方向</li>
 *       <li>内存编排的最小单位是byte 首先定义成byte是足够用了 其次要看到底用几bit
 *         <ul>
 *           <li>1bit可以表达2种类型</li>
 *           <li>2bit可以表达4种类型</li>
 *           <li>3bit可以表达8种类型</li>
 *         </ul>
 *       </li>
 *     </ul>
 *   </li>
 * </ul>
 *
 * 有了flags的用途再看当前的情况
 * 表达长度的成员len的类型可以定义成8bit 16bit 32bit 64bit
 * 因此与之对应的sds的类型就有4种sdshdr8 sdshdr16 sdshdr32 sdshdr64
 * 也就是说flags的8bit只要用到低位的2bit就足够了
 * 我的猜想是作者觉得剩余的6bit浪费掉太可惜了因此想利用起来
 * 6bit能够表达的sizet是[0...2^6)
 * 上面分析过sds结构成员len alloc flags buf 功能完备性必不可少的是len flags buf
 * alloc的用途是性能优化 减少了字符串伸缩时候产生的内存重分配情况
 * 因此如果有这样一个场景 字符串定义好之后就不发生变化或者很少发生伸缩变化
 * 那我是不是可以把alloc放弃掉 然后把len信息跟类型信息一起编到flags中去
 * 这就是sdshdr5的诞生吧
 * 此时在4种sds类型基础上额外增加了一种sds类型
 * 那么flags的低2位明显不够用了 至少得用低3位来表达类型 剩下的高5bit来表达长度
 *
 * <ul>
 *   <li>每种sds字符串类型名称的数字后缀就是sds能表达的字符串长度使用的bit
 *     <ul>
 *       <li>sds5 就是5bit长度上限 [0...2^5)</li>
 *       <li>sds8 就是8bit长度上限 (2^5...2^8)</li>
 *       <li>sds16 就是16bit长度上限 (2^8...2^16)</li>
 *       <li>sds32 就是32bit长度上限 (2^16...2^32)</li>
 *       <li>sds64 就是64bit长度上限 (2^32...2^64)</li>
 *     </ul>
 *   </li>
 *   <li>sds向OS申请到的实际内存大小存放在alloc中</li>
 *   <li>buf就是sds向OS申请到的内存</li>
 *   <li>sds字符串长度用len表达</li>
 *   <li>sds为了兼容C字符串 因此sds字符串也存了结束符\0 即buf中实际使用的就是len+1</li>
 *   <li>sds5没有存alloc信息 也就是说虽然实际上可能buf还有可用空间 但是因为没存储alloc所以就不知情 因此发生sds字符串伸缩都要重新创建实例</li>
 *   <li>redis设计出sds5肯定不是为了炫技 而是说真的有地方要使用 那么什么地方用到的字符串长度在64以下并且不发生字符串伸缩变化呢 应该就是key了吧</li>
 * </ul>
 */
struct __attribute__ ((__packed__)) sdshdr5 {
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; /* used */
	/**
	 * buf缓冲区的可用空间 整个buf中除了C字符串结束符剩下的空间
	 * 分配给sds的整个内存包括
	 * <ul>
	 *   <li>除去header的内存占用就是buf数组</li>
	 *   <li>buf数组是C字符串 因此字符串结束符占1个byte</li>
	 * </ul>
	 */
    uint8_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

// flags的5中类型
#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
// flags的低3位掩码 0x7
#define SDS_TYPE_MASK 7
#define SDS_TYPE_BITS 3
/**
 * 将指针sh指向sds数据结构
 * 已知sds字符串实例 本质是指针指向了sds的buf数组 计算对应的sds的head大小移动指针到sds的地址起始处
 * @param T 5 8 16 32 64 用于宏拼接参数
 * @param s sds实例指针 指向的是sds的buf数组
 */
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));
/**
 * sdshdr数据结构的最后一个成员是柔性数组
 * sdshrd的大小就是sds头的大小
 * 有了指向buf数组的指针 往低地址空间移动sdshdr头大小 指针就指向了sdshdr
 * 再根据sdshdr类型对指针进行类型强转得到sds的头
 * 想要sdshdr里面什么信息都可以
 * @param T sdshdr类型后缀 5 8 16 32 64 在宏中通过##把两个宏餐素好拼接起来
 * @param s sds字符串指针 也就是指向sds数据结构中buf的指针
 */
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T))))
/**
 * sdshdr5为了更省内存
 * flags的高5bit是字符床长度 低3bit是字符串类型
 * 因此flags右移3位就得到了字符串长度
 */
#define SDS_TYPE_5_LEN(f) ((f)>>SDS_TYPE_BITS)

/**
 * 计算sds字符串的长度
 * sds字符串的长度信息存储在sds头中 sds有5种不同的数据结构
 * 因此先拿到sds的类型标识 再根据不同的类型取成员信息
 * @param s sds字符串
 * @return sds字符串的长度
 */
static inline size_t sdslen(const sds s) {
	/**
	 *  sds暴露给用的指针指向的buf数组 sds的内存布局是buf前1个byte是flags
	 *  该flags高5bit是预留位 低3bit是类型标识
	 */
    unsigned char flags = s[-1];
	/**
	 * 只要flags的低3位标识信息 因此跟0b0111即0x07求&位运算
	 */
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags); // sdshdr5的字符串长度存储在flags的高5bit中
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->len;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->len;
    }
    return 0;
}

/**
 * sds5没有编排alloc信息
 * sds5本身定位就是用来sds字符串不变更的场景 换言之sds5发生变更就要创建新的sds实例
 * 因此sds5的剩余可用空间就是0
 * 其他类型的就是len-alloc
 * @param s sds指针 实际指向的是sds中的buf数组
 * @return sds中buf还剩余的可用空间 C字符串的结束符占的位置被归为可用空间
 */
static inline size_t sdsavail(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5: {
            return 0;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}

/**
 * 设置sds字符串长度 就是修改sdshdr中的len成员
 * @param s sds实例 本质是指向sds中buf的指针
 * @param newlen sds字符串新的长度
 */
static inline void sdssetlen(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
	    /**
	     * 依然是sds5需要单独处理 修改flags的高5位内容就行
	     * 其他都是拿到sdshdr结构体指就行
	     */
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len = newlen;
            break;
    }
}

/**
 * sds字符串长度自增
 * @param s sds字符串实例 实质指向的是sds的buf数组
 * @param inc 要自增的步进值
 */
static inline void sdsinclen(sds s, size_t inc) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
	    /**
	     * sds5类型的单独处理 拿出flags的高5位进行计算
	     * 其他类型的sds拿到sdshdr结构体对len成员进行计算
	     */
        case SDS_TYPE_5:
            {
			    // fp指向flags 修改高5位的值
                unsigned char *fp = ((unsigned char*)s)-1;
                unsigned char newlen = SDS_TYPE_5_LEN(flags)+inc;
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len += inc;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len += inc;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len += inc;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len += inc;
            break;
    }
}

/**
 * sds字符串中维护的alloc值
 * OS的内存分配器实际分配的内存大小
 * 这个值存储在sds头中的alloc成员
 * sds5特殊处理 因为没有alloc成员 所以姑且认为OS内存分配器实际分配的内存就是字符串长度
 * @param s sds字符串实例
 * @return sds字符串的alloc值
 */
/* sdsalloc() = sdsavail() + sdslen() */
static inline size_t sdsalloc(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->alloc;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->alloc;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->alloc;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->alloc;
    }
    return 0;
}

/**
 * sds的alloc成员赋值
 * sds5没有维护alloc成员 所以不需要处理
 * @param s sds实例
 */
static inline void sdssetalloc(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->alloc = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->alloc = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->alloc = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->alloc = newlen;
            break;
    }
}

/**
 * 根据C字符串创建sds字符串
 * @param init C字符串
 * @param initlen C字符串长度
 * @return sds字符串实例 指向的sds的buf数组
 */
sds sdsnewlen(const void *init, size_t initlen);

/**
 * 根据C字符串创建sds字符串
 * @param init C字符串
 * @param initlen C字符串长度
 * @return sds字符串实例 指向的sds的buf数组
 */
sds sdstrynewlen(const void *init, size_t initlen);

/**
 * 根据C字符串创建sds字符串
 * @param init C字符串
 * @return sds字符串实例 指向的sds的buf数组
 */
sds sdsnew(const char *init);

/**
 * 创建空字符串
 * 创建的是sds8类型的字符串
 * @return sds8类型的字符串实例
 */
sds sdsempty(void);

/**
 * sds字符串拷贝
 * @param s 源sds字符串 因为sds实例的指针实际指向的是buf数组 因此完全兼容C字符串 直接调用根据C字符串创建sds字符串的API就行
 * @return sds实例
 */
sds sdsdup(const sds s);

/**
 * 释放sds内存
 * @param s 要释放内存的sds实例
 */
void sdsfree(sds s);

/**
 * sds字符串长度至少为len
 * @param s sds字符串
 * @param len 要sds字符串长度满足的要求
 * @return sds字符串
 */
sds sdsgrowzero(sds s, size_t len);

/**
 * 将C字符串指定长度拼接到sds字符串上 追加拷贝
 * @param s sds字符串
 * @param t C字符串
 * @param len 将C字符串中多长拷贝到sds中
 * @return sds字符串
 */
sds sdscatlen(sds s, const void *t, size_t len);

/**
 * 将C字符串指定长度拼接到sds字符串上 追加拷贝
 * @param s sds字符串
 * @param t C字符串 把整个C字符串追加到sds上
 * @return sds字符串
 */
sds sdscat(sds s, const char *t);

/**
 * sds字符串追加到sds字符串上
 * @param s sds字符串
 * @param t 要把哪个sds字符串追加到别的sds字符串上
 * @return sds字符串
 */
sds sdscatsds(sds s, const sds t);

/**
 * 将C字符串中指定长度的子串拷贝到sds中 不是追加
 * @param s sds实例
 * @param t C字符串
 * @param len 要拷贝的C字符串长度
 * @return sds实例
 */
sds sdscpylen(sds s, const char *t, size_t len);

/**
 * 将C字符串拷贝到sds字符串中 不是追加
 * @param s sds实例 C字符串拷贝到哪儿
 * @param t C字符串
 * @return sds字符串
 */
sds sdscpy(sds s, const char *t);

/**
 * 格式化输出
 * @param s 格式化的结果拼接到这个sds字符串上
 * @param fmt 格式化
 * @param ap 格式化的参数列表
 * @return 拼接完的sds字符串
 */
sds sdscatvprintf(sds s, const char *fmt, va_list ap);

/**
 * 格式化输出的结果拼接到sds字符串
 * @param s 格式化输出的结果拼接到哪个sds字符串上
 * @param fmt 格式化字符串
 * @param ... 格式化字符串的参数列表
 * @return 拼接了格式化字符串的结果
 */
#ifdef __GNUC__
/**
 * __attribute__告诉编译器按照C标准库的printf函数对函数的参数进行检查
 * 2: 表示第2个参数为格式化字符串
 * 3: 表示第3个参数为格式化字符串的参数列表
 */
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

/**
 * 解析格式化字符串 拼接到s字符串
 * @param s 格式化输出的结果拼接到哪个sds字符串上
 * @param fmt 格式化字符串 C字符串
 * @param ... 格式化字符串的参数列表
 * @return 拼接了格式化字符串的结果
 */
sds sdscatfmt(sds s, char const *fmt, ...);

/**
 * 清掉sds字符串两头在cset中的内容
 * @param s sds字符串
 * @param cset 模式
 * @return 清除之后的sds字符串
 */
sds sdstrim(sds s, const char *cset);

/**
 * 子串
 * @param s 源sds字符串
 * @param start 开始位置
 * @param len 子串长度
 */
void sdssubstr(sds s, size_t start, size_t len);

/**
 * 截串
 * @param s 要截断哪个字符串 [start...end]左右都是闭区间
 * @param start 开始角标 负1表示最后一个位置 以此类推
 * @param end 开始角标 负1表示最后一个位置 以此类推
 */
void sdsrange(sds s, ssize_t start, ssize_t end);

/**
 * 根据C字符串结束符更新sds字符串长度
 * 可能是sds字符串创建好后 用户手动重置了buf数组中的C字符串结束符
 * 因此要该API同步对应字符串的长度
 * @param s sds字符串
 */
void sdsupdatelen(sds s);

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
void sdsclear(sds s);

/**
 * 字典序比较
 * @return 0 二者相等
 *         -1 s1<s2
 *         1 s1>s2
 */
int sdscmp(const sds s1, const sds s2);

/**
 * 给定分隔符进行字符串分割 分割的子串创建sds实例放到数组中
 * @param s 要分割的字符串
 * @param len 待分割的字符串长度
 * @param sep 分隔符
 * @param seplen 分隔符长度
 * @param count 源字符串被分割成了几个子串
 * @return 分割好的子串放到数组中
 */
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count);
void sdsfreesplitres(sds *tokens, int count);

/**
 * 字符串转小写
 * @param s sds字符串
 */
void sdstolower(sds s);

/**
 * 字符串转大写
 * @param s sds字符串
 */
void sdstoupper(sds s);

/**
 * 根据数字创建sds字符串
 * @param value 数字
 * @return sds字符串
 */
sds sdsfromlonglong(long long value);
sds sdscatrepr(sds s, const char *p, size_t len);
sds *sdssplitargs(const char *line, int *argc);
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);
sds sdsjoin(char **argv, int argc, char *sep);
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen);

/* Callback for sdstemplate. The function gets called by sdstemplate
 * every time a variable needs to be expanded. The variable name is
 * provided as variable, and the callback is expected to return a
 * substitution value. Returning a NULL indicates an error.
 */
typedef sds (*sdstemplate_callback_t)(const sds variable, void *arg);
sds sdstemplate(const char *template, sdstemplate_callback_t cb_func, void *cb_arg);

/* Low level functions exposed to the user API */
/**
 * 扩展sds的可用内存 保证sds字符串buf预分配的可用内存至少得有addlen个byte
 * @param s sds实例
 * @param addlen sds字符串buf可用的边界
 * @return sds实例 可能还是之前字符串 也可能是扩容过buf内存的那个字符串 更甚就是个新的sds字符串
 */
sds sdsMakeRoomFor(sds s, size_t addlen);

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
void sdsIncrLen(sds s, ssize_t incr);

/**
 * sds动态内存的体现 释放sds中未使用的空间 达到节省内存的效果
 * @param s sds实例
 * @return 释放内存空间之后的sds实例 可能还是之前的sds字符串 也可能还是跟之前同类型的sds字符串 也可又能是一个降级类型的字符串
 */
sds sdsRemoveFreeSpace(sds s);

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
size_t sdsAllocSize(sds s);

/**
 * sds的指针暴露给用户的指向buf数组 移动到header上
 * @param s sds实例
 * @return 指向sds的header的指针
 */
void *sdsAllocPtr(sds s);

/* Export the allocator used by SDS to the program using SDS.
 * Sometimes the program SDS is linked to, may use a different set of
 * allocators, but may want to allocate or free things that SDS will
 * respectively free or allocate. */
void *sds_malloc(size_t size);
void *sds_realloc(void *ptr, size_t size);
void sds_free(void *ptr);

#ifdef REDIS_TEST
int sdsTest(int argc, char *argv[], int accurate);
#endif

#endif
