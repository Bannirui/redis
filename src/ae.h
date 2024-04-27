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
 * 在IO任务中读写顺序的屏障标识
 * 正常情况下一个IO任务
 * <ul>
 *   <li>要么关注可读事件</li>
 *   <li>要么关注可写事件</li>
 * </ul>
 * 那为什么要增加这么个标识呢 通过这个标识将来在收到可读可写的通知时可以干预读写顺序
 * 由默认的先读后写转换成先写后读
 */
#define AE_BARRIER 4    /* With WRITABLE, never fire the event if the
                           READABLE event already fired in the same event
                           loop iteration. Useful when you want to persist
                           things to disk before sending replies, and want
                           to do that in a group fashion. */

/**
 * 给事件划分类型 因为核心是借助系统的多路复用器 因此IO事件的调度是托管给多路复用器的
 * 但是系统难免会有任务或者定时任务需要管理 将调度时机穿插在多路复用器的阻塞/唤醒时机上
 * 既可以享受多路复用器的高效实现 又可以利用调度时机 平衡CPU和IO的开销
 * <ul>
 *   <li>1 调度的任务类型是IO任务</li>
 *   <li>2 调度的任务类型是定时任务<ul>
 *     <li>一次性任务</li>
 *     <li>定时任务</li>
 *   </ul>3 调度的任务类型既包含IO任务又包含定时任务</li>
 *   <li>4 多路复用器poll调用形式非阻塞式</li>
 *   <li>8 在多路复用器阻塞前执行回调</li>
 *   <li>16 在多路复用器阻塞唤醒后执行回调</li>
 * </ul>
 * 作为任务调度的调度策略使用
 */
// 需要调度的任务类型是IO任务
#define AE_FILE_EVENTS (1<<0)
// 需要调度的任务类型是定时任务
#define AE_TIME_EVENTS (1<<1)
// 需要调度的任务类型既包括IO任务又包括定时任务
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
// 多路复用器poll调用形式是非阻塞式
#define AE_DONT_WAIT (1<<2)
// 允许在多路复用器阻塞前进行回调
#define AE_CALL_BEFORE_SLEEP (1<<3)
// 允许在多路复用器阻塞唤醒后进行回调
#define AE_CALL_AFTER_SLEEP (1<<4)

/**
 * 标识定时任务的性质
 * 在事件回调函数执行后返回该值标识当前定时任务是不是还需要继续被调度执行
 * 即
 * <ul>
 *   <li>-1 标识定时任务是个普通任务 不是周期任务</li>
 * </ul>
 */
#define AE_NOMORE -1
/**
 * 删除标记
 * timeEventHead链表上挂载着定时任务
 * 每个任务都有唯一id
 * id值是-1用于标识待删除
 * 典型惰性设计 在使用的时候校验一下id 发现是-1的时候再移除
 */
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
/**
 * fd可读可写就绪后通过回调IO任务处理器执行业务逻辑
 * @param clientData 回调函数执行的时候可能需要一些数据
 * @param mask 就绪fd的就绪状态 是可读还是可写
 */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
/**
 * 定时任务处理器
 * @param id 定时任务的唯一标识id 在timeEventHead链表中存储的时间事件 都有唯一id
 * @param clientData 回调函数执行的时候可能需要一些数据
 * @return 定时任务类型标识符 用于区分定时任务是一次性任务还是周期性任务
 *         <ul>
 *           <li>-1 标识定时任务是一次性任务 只要被调度执行一次即可 不是个周期性任务 执行完毕之后将id置为-1标识逻辑删除 下次遍历到的时候执行物理删除</li>
 *           <li>n 标识定时任务是个周期性性定时任务 执行窗口间隔是n秒钟 也就是说下一次调度时机是+n秒 重置该任务的执行时机 等待再一次被调度到</li>
 *         </ul>
 */
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
// IO任务
typedef struct aeFileEvent {
	/**
     * 监听该fd的什么类型IO事件 也就是多路复用器要监听的事件类型
     * <ul>
     *   <li>1 可读事件</li>
     *   <li>2 可写事件</li>
     *   <li>4 将来读写顺序是先写后读</li>
     * </ul>
	 */
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER) */
    // fd可读时 要回调的处理器
    aeFileProc *rfileProc;
    // fd可写时 要回调的处理器
    aeFileProc *wfileProc;
    /**
     * 回调处理器可以使用的数据
     */
    void *clientData;
} aeFileEvent;

/* Time event structure */
 /**
  * 定时任务 按照类型分为2种
  * <ul>
  *   <li>一次性定时任务</li>
  *   <li>周期性定时任务</li>
  * </ul>
  * 从数据结构上来看 redis对定时任务的管理确实有些简陋
  * <ul>
  *   <li>用双链表的数据结构</li>
  *   <li>没有按照定时任务的调度时间排序</li>
  * </ul>
  * 对比其他框架对定时任务的管理方式而言 redis的管理方式很粗旷 说明在redis总很少使用定时任务
  * <ul>
  *   <li>netty中使用的是优先级队列</li>
  *   <li>kafka中使用的是时间轮</li>
  * </ul>
  */
typedef struct aeTimeEvent {
    // 全局唯一id
    long long id; /* time event identifier. */
    /**
     * 定时任务要被调度执行的时间 时间单位是微秒
     * 也就是定时任务理论应该在什么时间被调度执行
     */
    monotime when;
    /**
     * 任务处理器
     * 也就是回调函数 在到达被调度的时间后回调该函数
     * 该函数返回值
     * <ul>
     *   <li>-1 标识该定时任务是个一次性任务 执行一次就不用再被调度了</li>
     *   <li>整数n 标识该定时任务是个周期性定时任务 执行周期是n毫秒</li>
     * </ul>
     */
    aeTimeProc *timeProc;
    /**
     * 析构处理器
     * 当回收aeTimeEvent内存时回调这个函数进行析构资源
     */
    aeEventFinalizerProc *finalizerProc;
	/**
	 * 在执行回调函数的时候可能需要某些数据 通过这种方式提前把需要的数据维护好 在使用的时候直接进行回调
	 * <ul>
	 *   <li>finalizerProc回调函数执行析构逻辑的时候可能需要使用数据</li>
	 *   <li>timeProc事件调度函数回调执行调度逻辑的时候可能需要使用数据</li>
	 *   <li></li>
	 * </ul>
	 */
    void *clientData;
	// 双链表的前驱节点
    struct aeTimeEvent *prev;
	// 双链表的后继节点
    struct aeTimeEvent *next;
	/**
	 * 引用计数
	 * 什么的引用计数呢 当事件被调度的时候计数自增
	 * 防止异常删除 比如某个场景
	 * <ul>
	 *   <li>事件x已经被调度执行 它的refcount计数会被增1</li>
	 *   <li>事件x还在执行中的时候 用户调用api对这个事件进行删除 它的id被置为-1</li>
	 *   <li>轮询这个事件的时候发现id=-1 准备执行物理删除</li>
	 *   <li>如果贸贸然直接删除事件 那么执行中的逻辑就会crash</li>
	 *   <li>所以通过refcount的设计保护这样的边界</li>
	 * </ul>
	 */
    int refcount; /* refcount to prevent timer events from being
  		   * freed in recursive time event calls. */
} aeTimeEvent;

/* A fired event */
/**
 * 就绪事件
 */
typedef struct aeFiredEvent {
	// 就绪事件主体
    int fd;
	/**
	 * 就绪状态
	 * <ul>
	 *   <li>1 可读</li>
	 *   <li>2 可写</li>
	 * </ul>
	 */
    int mask;
} aeFiredEvent;

/* State of an event based program */
/**
 * 事件循环器
 * 首先其本质是一个各种事件的管理器，其次借助系统多路复用器的回调时机有机整合这些事件的执行策略
 * <ul>
 *   <li>IO任务 socket</li>
 *   <li>定时任务<ul>
 *     <li>一次性定时任务</li>
 *     <li>周期性定时任务</li>
 *   </ul></li>
 * </ul>
 * 为了便于区别 姑且将事件循环器中的事件分为两种
 * <ul>
 *  <li>IO任务</li>
 *  <li>定时任务</li>
 * </ul>
 */
typedef struct aeEventLoop {
    /**
     * 记录事件管理器中注册在IO多路复用器上的最大的fd
     * 只针对IO任务进行记录
     * <ul>
     *   <li>-1的语义是eventLoop中没有管理IO任务</li>
     *   <li>非-1的语义是eventLoop中管理的IO任务的fd最大值</li>
     * </ul>
     * 作为事件管理器eventLoop缩容的边界条件
     */
    int maxfd;   /* highest file descriptor currently registered */
    // 事件管理器eventLoop的容量 redis中没有用hash表映射fd跟事件的关系 而是用数组维护 数组脚标就直接对应着fd
    int setsize; /* max number of file descriptors tracked */
    /**
     * 从命名就可以看出来这是维护定时任务的自增id
     * 向事件管理器添加新的定时任务时候分配一个唯一自增id给它
     */
    long long timeEventNextId;

	/**
	 * 任务数组 要托管系统多路复用器监听的事件集合
	 * 该数组以fd为脚标索引
	 * 将来知道了就绪IO事件之后 就可以反过来拿着fd来检索到注册时候的IO任务
	 */
    aeFileEvent *events; /* Registered events */

	/**
	 * 事件数组
	 * 存放就绪状态的事件
	 *
	 * 将系统多路复用器就绪的socket事件拷贝出来放到事件循环器中 跟用户层交互
	 * 也就是事件循环器屏蔽了用户层跟OS的直接交互
	 */
    aeFiredEvent *fired; /* Fired events */

    /**
     * 定时任务链表 用双链表进行组织 采用头插法
     * 定时任务分为
     * <ul>
     *   <li>一次性定时任务</li>
     *   <li>周期性定时任务</li>
     * </ul>
     * 不管那种细分类型的定时任务都放在这个双链表上 因此肯定会通过链表节点标识这个事件任务是定时任务还是周期任务
     */
    aeTimeEvent *timeEventHead;

	/**
	 * 状态标识符.
	 * 标识整个eventLoop的工作状态
	 * <ul>
	 *   <li>1-停止</li>
	 * </ul>
	 */
    int stop;

    /**
     * 事件循环器的核心所在
     * 处理socket是核心
     * 而高效处理socket的手段是利用OS的多路复用器 并且再巧妙借助多路复用器的回调时机巧妙衔接非socket任务
     * 因此作为一个事件循环器必须得持有一个OS的多路复用器实例
     * 在java中的方案就是再抽象一个基类 兼容不同系统平台的多路复用器具体实现
     * 在c中就比较简单 指针的魅力再次体现
     * 其本质是一样的 只持有多路复用器的实例 并不关注复用器的具体实现
     */
    void *apidata; /* This is used for polling API specific data */

    // 回调接口 在可能阻塞发生的IO复用器调用之前 执行回调
    aeBeforeSleepProc *beforesleep;
    // 回调接口 在IO复用器调用之后 执行回调
    aeBeforeSleepProc *aftersleep;
	/**
	 * 调用多路复用器poll行为是否阻塞 换言之超时多久
	 * int类型32位 bit位标识能力项和状态
	 * <ul>
	 *   <li>4 标识不阻塞 换言之timeout时间是0</li>
	 * </ul>
	 */
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
/**
 * eventLoop启动
 */
void aeMain(aeEventLoop *eventLoop);
/**
 * 对多路复用器进行了跨平台的封装
 * <ul>
 *   <li>mac的kqueue</li>
 *   <li>linux的epoll</li>
 * </ul>
 */
char *aeGetApiName(void);
/**
 * setter函数 指定回调函数
 */
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
/**
 * setter函数 指定回调函数
 */
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
