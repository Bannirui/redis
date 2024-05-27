/* Background I/O service for Redis.
 *
 * This file implements operations that we need to perform in the background.
 * Currently there is only a single operation, that is a background close(2)
 * system call. This is needed as when the process is the last owner of a
 * reference to a file closing it means unlinking it, and the deletion of the
 * file is slow, blocking the server.
 *
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put here Redis specific background tasks (for instance
 * it is not impossible that we'll need a non blocking FLUSHDB/FLUSHALL
 * implementation).
 *
 * DESIGN
 * ------
 *
 * The design is trivial, we have a structure representing a job to perform
 * and a different thread and job queue for every job type.
 * Every thread waits for new jobs in its queue, and process every job
 * sequentially.
 *
 * Jobs of the same type are guaranteed to be processed from the least
 * recently inserted to the most recently inserted (older jobs processed
 * first).
 *
 * Currently there is no way for the creator of the job to be notified about
 * the completion of the operation, this will only be added when/if needed.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include "server.h"
#include "bio.h"

/**
 * 后台线程的线程池
 * 存放了3个线程 每个线程分工不同 分工的依据就是线程池数组的脚标
 * <ul>
 *   <li>脚标0 BIO_CLOSE_FILE</li>
 *   <li>脚标1 BIO_AOF_FSYNC</li>
 *   <li>脚标2 BIO_LAZY_FREE</li>
 * </ul>
 * 怎么去标识一个线程呢 调用pthread_create实例化线程的时候系统会给每一个线程分配一个独一无二的id
 * 用这个id来作为线程的唯一标识
 */
static pthread_t bio_threads[BIO_NUM_OPS];
/**
 * 缓存了互斥锁
 * 线程池中3个线程 每个线程一把互斥锁
 */
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];
// 缓存线程池中线程对应的条件变量
static pthread_cond_t bio_newjob_cond[BIO_NUM_OPS];
// 缓存线程池中线程对应的条件变量
static pthread_cond_t bio_step_cond[BIO_NUM_OPS];
/**
 * 任务队列
 * 3个任务队列 对应线程池bio_threads中3个线程
 * 每个线程一个任务队列 队列的实现是链表
 */
static list *bio_jobs[BIO_NUM_OPS];
/* The following array is used to hold the number of pending jobs for every
 * OP type. This allows us to export the bioPendingJobsOfType() API that is
 * useful when the main thread wants to perform some operation that may involve
 * objects shared with the background thread. The main thread will just wait
 * that there are no longer jobs of this type to be executed before performing
 * the sensible operation. This data is also useful for reporting. */
/**
 * 任务队列中有多少个待处理的任务
 * 任务队列bio_jobs中缓存了每个后台线程要处理的任务
 * 任务队列的实现是基于链表 如果想知道任务的数量就需要遍历链表 时间复杂度是O(n)
 * 因此将任务队列的数据规模缓存起来
 * 并且增减操作是通过互斥锁来保证共享资源安全的
 */
static unsigned long long bio_pending[BIO_NUM_OPS];

/* This structure represents a background Job. It is only used locally to this
 * file as the API does not expose the internals at all. */
struct bio_job {
    time_t time; /* Time at which the job was created. */
    /* Job specific arguments.*/
    int fd; /* Fd for file based background jobs */
    lazy_free_fn *free_fn; /* Function that will free the provided arguments */
    void *free_args[]; /* List of arguments to be passed to the free function */
};

void *bioProcessBackgroundJobs(void *arg);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/* Initialize the background system, spawning the thread. */
/**
 * 初始化线程池的线程
 * 3个线程
 */
void bioInit(void) {
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;
    int j;

    /* Initialization of state vars and objects */
    for (j = 0; j < BIO_NUM_OPS; j++) {
		/**
		 * 初始化互斥锁 将锁缓存在bio_mutex数组中
		 * 要创建3个后台线程放在bio_threads线程池中
		 * 系统调用的参数
		 * <ul>
		 *  <li>mutex 指针 指向要初始化的互斥锁</li>
		 *  <li>attr 指针 指向的类型是pthread_mutexattr_t 用于指定互斥锁的属性 传入NULL表示使用默认属性</li>
		 * </ul>
		 */
        pthread_mutex_init(&bio_mutex[j],NULL);
		/**
		 * 用于初始化条件变量 通常与互斥锁一起使用 用于线程之间的同步
		 * 系统调用的参数
		 * <ul>
		 *   <li>第一个参数 指向要初始化的条件变量</li>
		 *   <li>第二个参数 指定条件变量属性 传入NULL表示使用默认属性</li>
		 * </ul>
		 * 返回0表示初始化成功 返回非0为错误码表示初始化失败
		 */
        pthread_cond_init(&bio_newjob_cond[j],NULL);
        pthread_cond_init(&bio_step_cond[j],NULL);
		// 为个线程创建一个任务队列 任务队列的实现是链表
        bio_jobs[j] = listCreate();
        bio_pending[j] = 0;
    }

    /* Set the stack size as by default it may be small in some system */
    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr,&stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes */
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass the job ID the thread is
     * responsible of. */
    for (j = 0; j < BIO_NUM_OPS; j++) {
        void *arg = (void*)(unsigned long) j;
		/**
		 * 系统调用pthread_create创建线程
		 * <ul>参数
		 *   <li>thread 指向pthread_t类型变量的指针 用于存储新线程的标识符</li>
		 *   <li>attr 指向pthread_attr_t类型变量的指针 用于指定新线程的属性 通常情况传递NULL表示使用默认属性</li>
		 *   <li>start_routine 指向函数的指针 该函数用于新线程的入口点 该函数的签名必须是void* (*start_routine) (void*) 它接受一个void*类型的参数并返回一个void*类型指针</li>
		 *   <li>arg 传递给start_routine函数的参数 是一个void*类型指针</li>
		 * </ul>
		 */
        if (pthread_create(&thread,&attr,bioProcessBackgroundJobs,arg) != 0) {
            serverLog(LL_WARNING,"Fatal: Can't initialize Background Jobs.");
            exit(1);
        }
		// 新建的线程缓存到线程池
        bio_threads[j] = thread;
    }
}

/**
 * 任务入队
 * @param type 线程池bio_threads数组对应的脚标 用于索引哪个线程
 * @param job 要入队的任务
 */
void bioSubmitJob(int type, struct bio_job *job) {
    job->time = time(NULL);
	// 当前线程对互斥锁上锁
    pthread_mutex_lock(&bio_mutex[type]);
	/**
	 * 新增的任务加到对应线程的任务队列里
	 * 任务的入队操作就是对链表的尾插
	 */
    listAddNodeTail(bio_jobs[type],job);
	// 更新队列的数据规模 任务队列增加了一个任务
    bio_pending[type]++;
	/**
	 * 随机唤醒一个当初阻塞在条件队列上的线程 让它继续执行
	 * 典型的生产者消费者模型
	 * 有任务队列空了的时候工作线程会阻塞等待直到有任务到来
	 *
	 * pthread_cond_signal系统调用是POSIX线程库中用来发送信号给等待在条件变量上的一个线程的函数
	 * 作用是通知等待中的线程 某个条件可能已经变为真 以便它们可以继续执行
	 * 通常情况用来唤醒一个等待在条件变量上的线程 如果有多个线程等待在条件变量上 那么只有一个线程会被唤醒 被唤醒的线程会尝试重新获取相关的互斥锁 然后继续执行
	 */
    pthread_cond_signal(&bio_newjob_cond[type]);
	// 当前线程对互斥锁解锁
    pthread_mutex_unlock(&bio_mutex[type]);
}

/**
 * 给BIO_LAZY_FREE类型的线程创建任务提交到任务队列
 */
void bioCreateLazyFreeJob(lazy_free_fn free_fn, int arg_count, ...) {
    va_list valist;
    /* Allocate memory for the job structure and all required
     * arguments */
    struct bio_job *job = zmalloc(sizeof(*job) + sizeof(void *) * (arg_count));
    job->free_fn = free_fn;

    va_start(valist, arg_count);
    for (int i = 0; i < arg_count; i++) {
        job->free_args[i] = va_arg(valist, void *);
    }
    va_end(valist);
	// 任务入队
    bioSubmitJob(BIO_LAZY_FREE, job);
}

/**
 * 给BIO_CLOSE_FILE类型的线程创建任务提交到任务队列
 */
void bioCreateCloseJob(int fd) {
    struct bio_job *job = zmalloc(sizeof(*job));
    job->fd = fd;

	// 任务入队
    bioSubmitJob(BIO_CLOSE_FILE, job);
}

/**
 * 给BIO_AOF_FSYNC类型的线程创建任务提交到任务队列
 */
void bioCreateFsyncJob(int fd) {
    struct bio_job *job = zmalloc(sizeof(*job));
    job->fd = fd;

	// 任务入队
    bioSubmitJob(BIO_AOF_FSYNC, job);
}

/**
 * 后台线程被CPU调度起来后执行的入口
 * 在调用pthread_create创建线程的时候需要指定函数指针告诉内核 线程被CPU调度起来之后执行什么逻辑
 * 后台线程通过while死循环执行 阻塞/唤醒通过任务队列有没有任务来控制
 * @param arg 0 1或2 在创建线程的时候传给pthread_create 线程被调度起来后系统回调这个函数 再把参数传到当前函数 实现不同线程负责不同业务的效果
 *            <ul>
 *              <li>0 BIO_CLOSE_FILE</li>
 *              <li>1 BIO_AOF_FSYNC</li>
 *              <li>2 BIO_LAZY_FREE</li>
 *            </ul>
 */
void *bioProcessBackgroundJobs(void *arg) {
    struct bio_job *job;
    unsigned long type = (unsigned long) arg;
    sigset_t sigset;

    /* Check that the type is within the right interval. */
    if (type >= BIO_NUM_OPS) {
        serverLog(LL_WARNING,
            "Warning: bio thread started with wrong type %lu",type);
        return NULL;
    }

	// 为线程设置名称
    switch (type) {
    case BIO_CLOSE_FILE:
        redis_set_thread_title("bio_close_file");
        break;
    case BIO_AOF_FSYNC:
        redis_set_thread_title("bio_aof_fsync");
        break;
    case BIO_LAZY_FREE:
        redis_set_thread_title("bio_lazy_free");
        break;
    }

    redisSetCpuAffinity(server.bio_cpulist);

    makeThreadKillable();

	// 上锁 互斥锁
    pthread_mutex_lock(&bio_mutex[type]);
    /* Block SIGALRM so we are sure that only the main thread will
     * receive the watchdog signal. */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        serverLog(LL_WARNING,
            "Warning: can't mask SIGALRM in bio.c thread: %s", strerror(errno));

	// 后台线程被CPU调度起来之后就让它一直工作着 通过任务队列有没有任务的情况控制着工作线程的阻塞/唤起状态
    while(1) {
        listNode *ln;

        /* The loop always starts with the lock hold. */
        if (listLength(bio_jobs[type]) == 0) {
            pthread_cond_wait(&bio_newjob_cond[type],&bio_mutex[type]);
            continue;
        }
        /* Pop the job from the queue. */
		/**
		 * 3个后台线程一一对应任务队列
		 * 从链表头摘下任务进行处理 处理完后释放任务的内存
		 */
        ln = listFirst(bio_jobs[type]);
        job = ln->value;
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
		// 释放互斥锁
        pthread_mutex_unlock(&bio_mutex[type]);

        /* Process the job accordingly to its type. */
		/**
		 * 3个后台线程各司其职
		 * 各自只关注自己任务队列中的任务
		 */
        if (type == BIO_CLOSE_FILE) {
            close(job->fd);
        } else if (type == BIO_AOF_FSYNC) {
            /* The fd may be closed by main thread and reused for another
             * socket, pipe, or file. We just ignore these errno because
             * aof fsync did not really fail. */
            if (redis_fsync(job->fd) == -1 &&
                errno != EBADF && errno != EINVAL)
            {
                int last_status;
                atomicGet(server.aof_bio_fsync_status,last_status);
                atomicSet(server.aof_bio_fsync_status,C_ERR);
                atomicSet(server.aof_bio_fsync_errno,errno);
                if (last_status == C_OK) {
                    serverLog(LL_WARNING,
                        "Fail to fsync the AOF file: %s",strerror(errno));
                }
            } else {
                atomicSet(server.aof_bio_fsync_status,C_OK);
            }
        } else if (type == BIO_LAZY_FREE) {
            job->free_fn(job->free_args);
        } else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
		// 上锁 互斥锁
        pthread_mutex_lock(&bio_mutex[type]);
		// 线程任务已经执行完毕 任务出队
        listDelNode(bio_jobs[type],ln);
		// 同步更新任务队列数据规模
        bio_pending[type]--;

        /* Unblock threads blocked on bioWaitStepOfType() if any. */
		/**
		 * 唤醒所有阻塞在条件队列上的线程 唤醒它们重新竞争互斥锁
		 */
        pthread_cond_broadcast(&bio_step_cond[type]);
    }
}

/* Return the number of pending jobs of the specified type. */
/**
 * 后台线程还有多少个待处理的任务在任务队列中
 * @param type 3种后台线程 指定哪一种线程
 * @return 后台线程待处理的任务数量
 */
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
	// 上锁
    pthread_mutex_lock(&bio_mutex[type]);
	// 后台线程还有多少个待处理任务
    val = bio_pending[type];
	// 解锁
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* If there are pending jobs for the specified type, the function blocks
 * and waits that the next job was processed. Otherwise the function
 * does not block and returns ASAP.
 *
 * The function returns the number of jobs still to process of the
 * requested type.
 *
 * This function is useful when from another thread, we want to wait
 * a bio.c thread to do more work in a blocking way.
 */
unsigned long long bioWaitStepOfType(int type) {
    unsigned long long val;
	// 上锁
    pthread_mutex_lock(&bio_mutex[type]);
	// 任务队列中待后台线程处理的任务数量
    val = bio_pending[type];
	/**
	 * 这个地方没看懂 为啥要把查询线程挂起等唤醒
	 * 什么时候查询线程才会被唤醒
	 * 此时任务队列中是有任务待执行的 每当后台线程处理完一个任务它就会发送信号到条件变量唤醒这个查询线程
	 */
    if (val != 0) {
		// 当前线程挂起
        pthread_cond_wait(&bio_step_cond[type],&bio_mutex[type]);
        val = bio_pending[type];
    }
	// 解锁
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
/**
 * 取消掉后台线程
 */
void bioKillThreads(void) {
    int err, j;

    for (j = 0; j < BIO_NUM_OPS; j++) {
		/**
		 * 不能自己kill自己
		 * pthread_self函数返回的是当前线程的id
		 */
        if (bio_threads[j] == pthread_self()) continue;
		/**
		 * 通过pthread_cancel发送取消请求给后台线程来取消后台线程
		 */
        if (bio_threads[j] && pthread_cancel(bio_threads[j]) == 0) {
			/**
			 * pthread_cancel不是同步取消线程的
			 * 通过信号机制向目标线程发送取消请求 因此可以看成异步方式
			 * 这个地方就得阻塞在这等到目标线程真的被取消了
			 */
            if ((err = pthread_join(bio_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d can not be joined: %s",
                        j, strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d terminated",j);
            }
        }
    }
}
