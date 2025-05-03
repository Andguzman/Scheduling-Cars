
#define CE_THREADS_H

#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>

// Define thread states
typedef enum {
    CE_THREAD_READY,
    CE_THREAD_RUNNING,
    CE_THREAD_BLOCKED,
    CE_THREAD_TERMINATED
} CEThreadState;

// Thread ID type
typedef unsigned int CEthread_t;

// Thread attributes - keeps it simple for now
typedef struct {
    int detachstate;
    size_t stacksize;
    // Add more attributes as needed
} CEthread_attr_t;

// Thread control block
typedef struct CEThread {
    CEthread_t id;
    void *(*start_routine)(void*);
    void *arg;
    ucontext_t context;
    void *stack;
    size_t stack_size;
    void *retval;
    CEThreadState state;
    struct CEThread *next;
    struct CEThread *join_waiting; // Thread waiting for this one to finish
} CEThread;

// Mutex structure
typedef struct {
    int locked;
    CEthread_t owner;
    CEThread *waiting_threads;
} CEmutex_t;

// Condition variable structure
typedef struct {
    CEThread *waiting_threads;
} CEcond_t;

// Thread attributes initialization
int CEthread_attr_init(CEthread_attr_t *attr);
int CEthread_attr_destroy(CEthread_attr_t *attr);

// Thread creation and management
int CEthread_create(CEthread_t *thread, const CEthread_attr_t *attr,
                   void *(*start_routine)(void*), void *arg);
int CEthread_join(CEthread_t thread, void **retval);
void CEthread_exit(void *retval);
CEthread_t CEthread_self(void);

// Mutex functions
int CEmutex_init(CEmutex_t *mutex, const void *attr);
int CEmutex_destroy(CEmutex_t *mutex);
int CEmutex_lock(CEmutex_t *mutex);
int CEmutex_unlock(CEmutex_t *mutex);

// Condition variable functions
int CEcond_init(CEcond_t *cond, const void *attr);
int CEcond_destroy(CEcond_t *cond);
int CEcond_wait(CEcond_t *cond, CEmutex_t *mutex);
int CEcond_timedwait(CEcond_t *cond, CEmutex_t *mutex, const struct timespec *abstime);
int CEcond_signal(CEcond_t *cond);
int CEcond_broadcast(CEcond_t *cond);

// Library initialization and cleanup
void CEthread_lib_init(void);
void CEthread_lib_destroy(void);

// Scheduler functions
void CEthread_yield(void);
void CEthread_scheduler(void);
