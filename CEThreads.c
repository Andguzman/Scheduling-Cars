#include "CEThreads.h"
#include <string.h>
#include <unistd.h>
#include <time.h>

#define DEFAULT_STACK_SIZE (1024 * 1024) // 1MB stack
#define MAX_THREADS 1000
#ifndef SIGSTKSZ
#define SIGSTKSZ (8*1024) // 8KB default stack size for signals if not defined
#endif

// Global variables for thread management
static CEThread *thread_table[MAX_THREADS] = {NULL};
static CEthread_t next_thread_id = 1;
static CEThread *current_thread = NULL;
static CEThread *ready_queue = NULL;
static ucontext_t scheduler_context;
static int library_initialized = 0;
static char scheduler_stack[SIGSTKSZ];

// Forward declarations
static void thread_wrapper(void *thread_ptr);
static void add_to_ready_queue(CEThread *thread);
static CEThread *get_next_from_ready_queue(void);
static void remove_from_ready_queue(CEThread *thread);
void CEthread_scheduler(void);

// Initialize the thread library
void CEthread_lib_init(void) {
    if (library_initialized) return;

    // Create main thread context
    CEThread *main_thread = (CEThread*)malloc(sizeof(CEThread));
    if (!main_thread) {
        perror("Failed to allocate main thread");
        exit(EXIT_FAILURE);
    }

    main_thread->id = 0;  // ID 0 is reserved for main thread
    main_thread->state = CE_THREAD_RUNNING;
    main_thread->next = NULL;
    main_thread->join_waiting = NULL;
    main_thread->stack = NULL;  // Main thread uses system stack

    // Get current context for main thread
    if (getcontext(&main_thread->context) < 0) {
        perror("getcontext");
        free(main_thread);
        exit(EXIT_FAILURE);
    }

    thread_table[0] = main_thread;
    current_thread = main_thread;

    // Initialize scheduler context
    if (getcontext(&scheduler_context) < 0) {
        perror("getcontext for scheduler");
        exit(EXIT_FAILURE);
    }

    scheduler_context.uc_stack.ss_sp = scheduler_stack;
    scheduler_context.uc_stack.ss_size = SIGSTKSZ;
    scheduler_context.uc_link = NULL;

    makecontext(&scheduler_context, (void (*)())CEthread_scheduler, 0);

    library_initialized = 1;
}

// Clean up and destroy the thread library
void CEthread_lib_destroy(void) {
    if (!library_initialized) return;

    // Free all thread resources
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i]) {
            if (thread_table[i]->stack) {
                free(thread_table[i]->stack);
            }
            free(thread_table[i]);
            thread_table[i] = NULL;
        }
    }

    library_initialized = 0;
}

// Thread attributes initialization
int CEthread_attr_init(CEthread_attr_t *attr) {
    if (!attr) return EINVAL;

    attr->detachstate = 0; // Default to joinable
    attr->stacksize = DEFAULT_STACK_SIZE;

    return 0;
}

int CEthread_attr_destroy(CEthread_attr_t *attr) {
    if (!attr) return EINVAL;
    // Nothing to actually destroy
    return 0;
}

// Find a free slot in the thread table
static CEthread_t allocate_thread_id(void) {
    for (CEthread_t i = 1; i < MAX_THREADS; i++) {
        if (thread_table[i] == NULL) {
            return i;
        }
    }
    return 0; // No free slots
}

// Thread creation
int CEthread_create(CEthread_t *thread, const CEthread_attr_t *attr,
                   void *(*start_routine)(void*), void *arg) {
    if (!library_initialized) {
        CEthread_lib_init();
    }

    if (!thread || !start_routine) {
        return EINVAL;
    }

    // Allocate thread control block
    CEThread *new_thread = (CEThread*)malloc(sizeof(CEThread));
    if (!new_thread) {
        return ENOMEM;
    }

    // Allocate thread ID
    CEthread_t new_id = allocate_thread_id();
    if (new_id == 0) {
        free(new_thread);
        return EAGAIN;
    }

    // Initialize thread properties
    new_thread->id = new_id;
    new_thread->start_routine = start_routine;
    new_thread->arg = arg;
    new_thread->state = CE_THREAD_READY;
    new_thread->next = NULL;
    new_thread->join_waiting = NULL;
    new_thread->retval = NULL;

    // Get current context as a starting point
    if (getcontext(&new_thread->context) < 0) {
        perror("getcontext");
        free(new_thread);
        return EAGAIN;
    }

    // Allocate stack for the thread
    size_t stack_size = attr ? attr->stacksize : DEFAULT_STACK_SIZE;
    new_thread->stack = malloc(stack_size);
    if (!new_thread->stack) {
        free(new_thread);
        return ENOMEM;
    }
    new_thread->stack_size = stack_size;

    // Set up the context for the new thread
    new_thread->context.uc_stack.ss_sp = new_thread->stack;
    new_thread->context.uc_stack.ss_size = stack_size;
    new_thread->context.uc_link = &scheduler_context;

    // Set up the thread to run the wrapper function with the thread as parameter
    makecontext(&new_thread->context, (void (*)())thread_wrapper, 1, new_thread);

    // Store thread in thread table
    thread_table[new_id] = new_thread;
    *thread = new_id;

    // Add to ready queue
    add_to_ready_queue(new_thread);

    return 0;
}

// The wrapper function that all threads execute
static void thread_wrapper(void *thread_ptr) {
    // Get thread from parameter instead of global
    CEThread *thread = (CEThread*)thread_ptr;

    // Double-check that we have the correct thread
    if (!thread || thread != current_thread) {
        fprintf(stderr, "Thread wrapper received incorrect thread pointer\n");
        exit(EXIT_FAILURE);
    }

    // Execute the thread function
    void *retval = thread->start_routine(thread->arg);

    // Thread is done
    thread->retval = retval;
    thread->state = CE_THREAD_TERMINATED;

    // Wake up any thread waiting to join with this one
    if (thread->join_waiting) {
        thread->join_waiting->state = CE_THREAD_READY;
        add_to_ready_queue(thread->join_waiting);
        thread->join_waiting = NULL;
    }

    // Schedule next thread
    setcontext(&scheduler_context);

    // We should never reach here
    fprintf(stderr, "Thread wrapper: Failed to switch to scheduler\n");
    exit(EXIT_FAILURE);
}

// Wait for a thread to terminate
int CEthread_join(CEthread_t thread_id, void **retval) {
    if (!library_initialized) {
        return EINVAL;
    }

    if (thread_id <= 0 || thread_id >= MAX_THREADS || !thread_table[thread_id]) {
        return ESRCH;
    }

    CEThread *thread = thread_table[thread_id];

    // Self-deadlock check
    if (thread->id == current_thread->id) {
        return EDEADLK;
    }

    // If thread already terminated, just collect its return value
    if (thread->state == CE_THREAD_TERMINATED) {
        if (retval) {
            *retval = thread->retval;
        }

        // Free resources
        free(thread->stack);
        free(thread);
        thread_table[thread_id] = NULL;

        return 0;
    }

    // Otherwise, block until the thread terminates
    current_thread->state = CE_THREAD_BLOCKED;
    thread->join_waiting = current_thread;

    // Switch to scheduler to run another thread
    swapcontext(&current_thread->context, &scheduler_context);

    // When we return here, the target thread has terminated

    if (retval) {
        *retval = thread->retval;
    }

    // Free resources
    free(thread->stack);
    free(thread);
    thread_table[thread_id] = NULL;

    return 0;
}

// Exit the current thread
void CEthread_exit(void *retval) {
    if (!current_thread) return;

    current_thread->retval = retval;
    current_thread->state = CE_THREAD_TERMINATED;

    // Wake up any thread waiting to join with this one
    if (current_thread->join_waiting) {
        current_thread->join_waiting->state = CE_THREAD_READY;
        add_to_ready_queue(current_thread->join_waiting);
        current_thread->join_waiting = NULL;
    }

    // Switch to scheduler
    setcontext(&scheduler_context);

    // We should never reach here
    fprintf(stderr, "CEthread_exit: Failed to switch to scheduler\n");
    exit(EXIT_FAILURE);
}

// Get the thread ID of the calling thread
CEthread_t CEthread_self(void) {
    if (!current_thread) return 0;
    return current_thread->id;
}

// Yield the CPU to another thread
void CEthread_yield(void) {
    if (!current_thread) return;

    CEThread *prev_thread = current_thread;

    // Only add back to ready queue if still active
    if (prev_thread->state == CE_THREAD_RUNNING) {
        prev_thread->state = CE_THREAD_READY;
        add_to_ready_queue(prev_thread);
    }

    // Switch to scheduler
    swapcontext(&prev_thread->context, &scheduler_context);
}

// The scheduler function
void CEthread_scheduler(void) {
    while (1) {
        // Get next thread from ready queue
        CEThread *next_thread = get_next_from_ready_queue();

        if (!next_thread) {
            // No threads ready to run
            if (current_thread && current_thread->state == CE_THREAD_RUNNING) {
                // Continue executing the current thread
                return;
            }

            // Check if there are any active threads
            int active_threads = 0;
            for (int i = 0; i < MAX_THREADS; i++) {
                if (thread_table[i] && thread_table[i]->state != CE_THREAD_TERMINATED) {
                    active_threads++;
                }
            }

            if (active_threads == 0) {
                // No more active threads, return to main thread if possible
                if (thread_table[0]) {
                    current_thread = thread_table[0];
                    current_thread->state = CE_THREAD_RUNNING;
                    setcontext(&current_thread->context);
                } else {
                    fprintf(stderr, "CEthread_scheduler: No more active threads\n");
                    exit(EXIT_SUCCESS);
                }
            }

            // Wait for a thread to become ready
            usleep(1000); // Sleep for 1ms to avoid busy waiting
            continue;
        }

        // Switch to the next thread
        CEThread *prev_thread = current_thread;
        current_thread = next_thread;
        current_thread->state = CE_THREAD_RUNNING;

        if (prev_thread == NULL) {
            // First time scheduling
            setcontext(&current_thread->context);
        } else if (prev_thread->state == CE_THREAD_TERMINATED) {
            // Previous thread is terminated, no need to save its context
            setcontext(&current_thread->context);
        } else {
            // Switch contexts
            swapcontext(&scheduler_context, &current_thread->context);
        }
    }
}

// Queue management functions
static void add_to_ready_queue(CEThread *thread) {
    if (!thread) return;

    thread->next = NULL;

    if (!ready_queue) {
        ready_queue = thread;
    } else {
        CEThread *temp = ready_queue;
        while (temp->next) {
            temp = temp->next;
        }
        temp->next = thread;
    }
}

static CEThread *get_next_from_ready_queue(void) {
    if (!ready_queue) return NULL;

    CEThread *next = ready_queue;
    ready_queue = ready_queue->next;
    next->next = NULL;

    return next;
}

static void remove_from_ready_queue(CEThread *thread) {
    if (!ready_queue || !thread) return;

    if (ready_queue == thread) {
        ready_queue = ready_queue->next;
        return;
    }

    CEThread *temp = ready_queue;
    while (temp->next && temp->next != thread) {
        temp = temp->next;
    }

    if (temp->next == thread) {
        temp->next = thread->next;
    }
}

// Mutex functions
int CEmutex_init(CEmutex_t *mutex, const void *attr) {
    if (!mutex) return EINVAL;

    mutex->locked = 0;
    mutex->owner = 0;
    mutex->waiting_threads = NULL;

    return 0;
}

int CEmutex_destroy(CEmutex_t *mutex) {
    if (!mutex) return EINVAL;

    // Check if mutex is locked
    if (mutex->locked) {
        return EBUSY;
    }

    return 0;
}

int CEmutex_lock(CEmutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (!current_thread) return EINVAL;
    // Fast path: if mutex is unlocked, acquire it
    if (!mutex->locked) {
        mutex->locked = 1;
        mutex->owner = current_thread->id;
        return 0;
    }
    // Check for deadlock if we already own the mutex
    if (mutex->owner == current_thread->id) {
        return EDEADLK;
    }
    // Slow path: mutex is locked, wait for it
    current_thread->state = CE_THREAD_BLOCKED;

    // Add to mutex waiting list
    CEThread *temp = mutex->waiting_threads;
    if (!temp) {
        mutex->waiting_threads = current_thread;
    } else {
        while (temp->next) {
            temp = temp->next;
        }
        temp->next = current_thread;
    }
    current_thread->next = NULL;

    // Switch to scheduler
    swapcontext(&current_thread->context, &scheduler_context);

    // When we return here, we should have the mutex
    mutex->locked = 1;
    mutex->owner = current_thread->id;
    return 0;
}

int CEmutex_unlock(CEmutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (!current_thread) return EINVAL;
    // Check if mutex is locked
    if (!mutex->locked) {
        return EPERM;
    }

    // Check if current thread owns the mutex
    if (mutex->owner != current_thread->id) {
        return EPERM;
    }

    // Unlock mutex
    mutex->locked = 0;
    mutex->owner = 0;

    // Wake up one waiting thread
    if (mutex->waiting_threads) {
        CEThread *thread = mutex->waiting_threads;
        mutex->waiting_threads = thread->next;
        thread->next = NULL;
        thread->state = CE_THREAD_READY;
        add_to_ready_queue(thread);
    }

    return 0;
}

// Condition variable functions
int CEcond_init(CEcond_t *cond, const void *attr) {
    if (!cond) return EINVAL;

    cond->waiting_threads = NULL;

    return 0;
}

int CEcond_destroy(CEcond_t *cond) {
    if (!cond) return EINVAL;

    // Check if condition variable has waiting threads
    if (cond->waiting_threads) {
        return EBUSY;
    }

    return 0;
}

int CEcond_wait(CEcond_t *cond, CEmutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;
    if (!current_thread) return EINVAL;

    // Check if we own the mutex
    if (!mutex->locked || mutex->owner != current_thread->id) {
        return EPERM;
    }

    // Unlock mutex
    mutex->locked = 0;
    mutex->owner = 0;

    // Wake up one waiting thread on the mutex
    if (mutex->waiting_threads) {
        CEThread *thread = mutex->waiting_threads;
        mutex->waiting_threads = thread->next;
        thread->next = NULL;
        thread->state = CE_THREAD_READY;
        add_to_ready_queue(thread);
    }

    // Add ourselves to the condition variable waiting list
    current_thread->state = CE_THREAD_BLOCKED;
    current_thread->next = NULL;

    if (!cond->waiting_threads) {
        cond->waiting_threads = current_thread;
    } else {
        CEThread *temp = cond->waiting_threads;
        while (temp->next) {
            temp = temp->next;
        }
        temp->next = current_thread;
    }

    // Switch to scheduler
    swapcontext(&current_thread->context, &scheduler_context);

    // When we return here, we've been signaled
    // Reacquire the mutex
    return CEmutex_lock(mutex);
}

int CEcond_timedwait(CEcond_t *cond, CEmutex_t *mutex, const struct timespec *abstime) {
    if (!cond || !mutex || !abstime) return EINVAL;

    // This is a simplified implementation; in reality, you would need to
    // check the timeout and possibly wake up early

    // For now, just call regular wait (no timeout handling)
    // In a real implementation, you would set an alarm or use another mechanism
    return CEcond_wait(cond, mutex);
}

int CEcond_signal(CEcond_t *cond) {
    if (!cond) return EINVAL;

    // Wake up one waiting thread
    if (cond->waiting_threads) {
        CEThread *thread = cond->waiting_threads;
        cond->waiting_threads = thread->next;
        thread->next = NULL;
        thread->state = CE_THREAD_READY;
        add_to_ready_queue(thread);
    }

    return 0;
}

int CEcond_broadcast(CEcond_t *cond) {
    if (!cond) return EINVAL;

    // Wake up all waiting threads
    while (cond->waiting_threads) {
        CEThread *thread = cond->waiting_threads;
        cond->waiting_threads = thread->next;
        thread->next = NULL;
        thread->state = CE_THREAD_READY;
        add_to_ready_queue(thread);
    }

    return 0;
}