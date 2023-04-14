/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __AE_H__
#define __AE_H__

#include "monotonic.h"

#define AE_OK 0
#define AE_ERR -1

// 没有注册到IO复用器上
#define AE_NONE 0       /* No events registered. */
// 注册到IO复用器上 关注的是可读事件
#define AE_READABLE 1   /* Fire when descriptor is readable. */
// 注册到IO复用器上 关注的是可写事件
#define AE_WRITABLE 2   /* Fire when descriptor is writable. */
/**
 * 延迟处理
 * 在对某个fd进行删除关注事件时 如果彼时fd已经可写 会在那个时机标识上BAERIER
 * 如果某个fd的mask掩码包含了BARRIER 则该fd的处理顺序为先写后读
 */
#define AE_BARRIER 4    /* With WRITABLE, never fire the event if the
                           READABLE event already fired in the same event
                           loop iteration. Useful when you want to persist
                           things to disk before sending replies, and want
                           to do that in a group fashion. */

// 标识事件是文件事件
#define AE_FILE_EVENTS (1<<0)
// 标识事件是时间事件
#define AE_TIME_EVENTS (1<<1)
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT (1<<2)
#define AE_CALL_BEFORE_SLEEP (1<<3)
#define AE_CALL_AFTER_SLEEP (1<<4)

// 作为时间事件处理器的返回值 标识时间事件是个定时事件 执行一次就结束了
#define AE_NOMORE -1
// 时间事件id 标记删除
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
/**
 * fd可读可写就绪后通过回调文件事件处理器执行业务逻辑
 * @param clientData 文件事件的私有数据
 * @param mask 就绪fd的就绪状态 是可读还是可写
 */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
/**
 * @brief 时间事件处理器
 * @param id 时间事件id 在timeEventHead链表中存储的时间事件 都有唯一id
 * @param clientData 时间事件私有数据
 */
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
// 文件事件
typedef struct aeFileEvent {
    // 监听该fd的什么类型IO事件 也就是多路复用器要监听的事件类型
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER) */
    // fd可读时 要回调的处理器
    aeFileProc *rfileProc;
    // fd可写时 要回调的处理器
    aeFileProc *wfileProc;
    // 事件私有数据
    void *clientData;
} aeFileEvent;

/* Time event structure */
/**
 * @brief 时间事件
 *          - 定时事件
 *          - 周期性事件
 *        从数据结构上来看 事件的组织形式就是一条简单的双链表
 *        对比其他框架的实现
 *          - netty中使用的是优先级队列
 *          - kafka中使用的是时间轮
 *        redis中的这个管理显的很粗旷 说明在redis中很少有时间事件
 */
typedef struct aeTimeEvent {
    // 全局唯一id
    long long id; /* time event identifier. */
    /**
     * 事件到达时间 微秒
     * 也就是事件理论应该在什么事件被调度执行
     */
    monotime when;
    /**
     * 事件处理器
     * 定义了对该时间事件的业务处理逻辑
     * 该函数返回值
     *   - -1标识时间事件是个定时事件 执行一次就不用再被调度了 不是周期性事件
     *   - 整数n标识执行周期是n个毫秒
     */
    aeTimeProc *timeProc;
    /**
     * 事件的析构处理器
     * 事件处理结束后的回调接口 主要用来析构资源
     */
    aeEventFinalizerProc *finalizerProc;
    // 时间事件的私有数据
    void *clientData;
    struct aeTimeEvent *prev; // 前驱节点
    struct aeTimeEvent *next; // 后继节点
    int refcount; /* refcount to prevent timer events from being
  		   * freed in recursive time event calls. */
} aeTimeEvent;

/* A fired event */
typedef struct aeFiredEvent {
    int fd;
    int mask;
} aeFiredEvent;

/* State of an event based program */
/**
 * @brief 事件管理器
 *          - 时间事件
 *            - 定时事件
 *            - 周期性事件
 *          - 文件事件
 */
typedef struct aeEventLoop {
    /**
     * 记录事件管理器中注册在IO多路复用器上的最大的fd
     * 也就意味着只针对文件事件
     * 作为事件管理器缩容的边界条件
     */
    int maxfd;   /* highest file descriptor currently registered */
    // 事件管理器容量 redis中没有用hash表映射fd跟事件的关系 而是用数组维护 数组脚标就直接对应着fd
    int setsize; /* max number of file descriptors tracked */
    /**
     * 从命名就可以看出来这是维护时间事件的自增id
     * 向事件管理器添加新的时间事件的时候分配一个唯一自增id给它
     */
    long long timeEventNextId;
    // 文件事件列表 即托管在事件管理器里面的fd
    aeFileEvent *events; /* Registered events */
    // 就绪的文件事件列表 IO复用器系统调用的结果存储的地方
    aeFiredEvent *fired; /* Fired events */
    /**
     * @brief 时间事件列表 双链表
     *          - 定时事件
     *          - 周期性事件
     */
    aeTimeEvent *timeEventHead;
    int stop;
    /**
     * @brief 事件管理器持有OS的多路复用器
     *          - 各个OS对IO多路复用的实现不同
     *          - 通过aeApiState封装多路复用
     *          - 事件管理器持有aeApiState实例
     */
    void *apidata; /* This is used for polling API specific data */
    // 回调接口 在可能阻塞发生的IO复用器调用之前 执行回调
    aeBeforeSleepProc *beforesleep;
    // 回调接口 在IO复用器调用之后 执行回调
    aeBeforeSleepProc *aftersleep;
    int flags;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
