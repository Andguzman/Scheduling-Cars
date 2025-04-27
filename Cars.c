#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

// ---------------- CEThreads Library Implementation ----------------

#define STACK_SIZE 64*1024
#define MAX_THREADS 100
#define QUANTUM_USEC 100000 // 100ms time quantum for Round Robin

// Thread states
typedef enum {
    READY,
    RUNNING,
    BLOCKED,
    TERMINATED
} thread_state_t;

// Thread control block
typedef struct {
    int id;
    ucontext_t context;
    void* stack;
    void* (*start_routine)(void*);
    void* arg;
    thread_state_t state;
    int priority;          // For Priority scheduling
    int estimated_time;    // For SJF scheduling
    int deadline;          // For Real-time scheduling
    struct timespec creation_time; // For FCFS scheduling
    int joined_by;         // ID of thread waiting for this to complete
    void* retval;          // Return value
} cethread_t;

// Mutex implementation
typedef struct {
    int locked;
    int owner;
    int initialized;
} cemutex_t;

// Scheduler algorithm types
typedef enum {
    SCHEDULER_RR,       // Round Robin
    SCHEDULER_PRIO,     // Priority
    SCHEDULER_SJF,      // Shortest Job First
    SCHEDULER_FCFS,     // First Come First Served
    SCHEDULER_RT        // Real-Time
} scheduler_type_t;

// Global variables
static cethread_t thread_table[MAX_THREADS];
static int current_thread = -1;
static int thread_count = 0;
static int next_thread_id = 0;
static scheduler_type_t scheduler_algo = SCHEDULER_RR;
static ucontext_t scheduler_context;

// Function prototypes
void scheduler();
int select_next_thread();
void init_threads();

// Initialize thread system
void init_threads() {
    static int initialized = 0;
    if (initialized) return;

    // Initialize main thread
    thread_table[0].id = next_thread_id++;
    thread_table[0].state = RUNNING;
    thread_table[0].priority = 5;
    thread_table[0].estimated_time = 0;
    thread_table[0].deadline = 0;
    clock_gettime(CLOCK_REALTIME, &thread_table[0].creation_time);
    thread_table[0].joined_by = -1;
    thread_table[0].retval = NULL;
    thread_count = 1;
    current_thread = 0;

    // Create scheduler context
    getcontext(&scheduler_context);
    scheduler_context.uc_stack.ss_sp = malloc(STACK_SIZE);
    scheduler_context.uc_stack.ss_size = STACK_SIZE;
    scheduler_context.uc_link = NULL;
    makecontext(&scheduler_context, scheduler, 0);

    initialized = 1;
}

// Thread wrapper function
void thread_wrapper() {
    // Get current thread
    cethread_t* thread = &thread_table[current_thread];

    // Call the thread function with its argument
    void* ret = thread->start_routine(thread->arg);

    // Store return value
    thread->retval = ret;

    // Mark thread as terminated
    thread->state = TERMINATED;

    // Wake up any thread waiting for this one
    if (thread->joined_by >= 0) {
        thread_table[thread->joined_by].state = READY;
    }

    // Switch to scheduler
    swapcontext(&thread->context, &scheduler_context);
}

// Create a new thread
int CEthread_create(cethread_t* thread, void* (*start_routine)(void*), void* arg, int priority, int estimated_time, int deadline) {
    init_threads();

    if (thread_count >= MAX_THREADS) {
        return -1;
    }

    // Find empty slot
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == TERMINATED || i >= thread_count) {
            break;
        }
    }

    if (i >= MAX_THREADS) {
        return -1;
    }

    // Initialize thread
    thread_table[i].id = next_thread_id++;
    thread_table[i].start_routine = start_routine;
    thread_table[i].arg = arg;
    thread_table[i].state = READY;
    thread_table[i].priority = priority;
    thread_table[i].estimated_time = estimated_time;
    thread_table[i].deadline = deadline;
    clock_gettime(CLOCK_REALTIME, &thread_table[i].creation_time);
    thread_table[i].joined_by = -1;

    // Allocate stack
    thread_table[i].stack = malloc(STACK_SIZE);
    if (!thread_table[i].stack) {
        return -1;
    }

    // Initialize context
    getcontext(&thread_table[i].context);
    thread_table[i].context.uc_stack.ss_sp = thread_table[i].stack;
    thread_table[i].context.uc_stack.ss_size = STACK_SIZE;
    thread_table[i].context.uc_link = &scheduler_context;
    makecontext(&thread_table[i].context, thread_wrapper, 0);

    // Update thread count
    if (i >= thread_count) {
        thread_count = i + 1;
    }

    // Store thread ID
    if (thread) {
        *thread = thread_table[i];
    }

    return thread_table[i].id;
}

// Wait for thread termination
int CEthread_join(cethread_t thread, void** retval) {
    init_threads();

    // Find thread
    int i;
    for (i = 0; i < thread_count; i++) {
        if (thread_table[i].id == thread.id) {
            break;
        }
    }

    if (i >= thread_count) {
        return -1;
    }

    // If thread is already terminated, return immediately
    if (thread_table[i].state == TERMINATED) {
        if (retval) {
            *retval = thread_table[i].retval;
        }
        return 0;
    }

    // Mark current thread as waiting for thread i
    thread_table[i].joined_by = current_thread;
    thread_table[current_thread].state = BLOCKED;

    // Switch to scheduler
    swapcontext(&thread_table[current_thread].context, &scheduler_context);

    // We're back, thread must have terminated
    if (retval) {
        *retval = thread_table[i].retval;
    }

    return 0;
}

// Initialize mutex
int CEmutex_init(cemutex_t* mutex) {
    init_threads();

    mutex->locked = 0;
    mutex->owner = -1;
    mutex->initialized = 1;

    return 0;
}

// Destroy mutex
int CEmutex_destroy(cemutex_t* mutex) {
    if (!mutex->initialized) {
        return -1;
    }

    mutex->initialized = 0;

    return 0;
}

// Lock mutex
int CEmutex_lock(cemutex_t* mutex) {
    init_threads();

    if (!mutex->initialized) {
        return -1;
    }

    // If mutex is locked, block
    while (mutex->locked) {
        thread_table[current_thread].state = BLOCKED;
        swapcontext(&thread_table[current_thread].context, &scheduler_context);
    }

    // Lock mutex
    mutex->locked = 1;
    mutex->owner = current_thread;

    return 0;
}

// Unlock mutex
int CEmutex_unlock(cemutex_t* mutex) {
    init_threads();

    if (!mutex->initialized || mutex->owner != current_thread) {
        return -1;
    }

    // Unlock mutex
    mutex->locked = 0;
    mutex->owner = -1;

    // Wake up all blocked threads
    for (int i = 0; i < thread_count; i++) {
        if (thread_table[i].state == BLOCKED && i != current_thread) {
            thread_table[i].state = READY;
        }
    }

    return 0;
}

// Round Robin scheduler
int scheduler_rr() {
    if (current_thread < 0) return 0;

    int next = (current_thread + 1) % thread_count;
    while (next != current_thread) {
        if (thread_table[next].state == READY) {
            return next;
        }
        next = (next + 1) % thread_count;
    }

    return current_thread;
}

// Priority scheduler
int scheduler_priority() {
    int highest_prio = -1;
    int next = current_thread;

    for (int i = 0; i < thread_count; i++) {
        if (thread_table[i].state == READY &&
            (highest_prio == -1 || thread_table[i].priority > highest_prio)) {
            highest_prio = thread_table[i].priority;
            next = i;
        }
    }

    return next;
}

// Shortest Job First scheduler
int scheduler_sjf() {
    int shortest_time = -1;
    int next = current_thread;

    for (int i = 0; i < thread_count; i++) {
        if (thread_table[i].state == READY &&
            (shortest_time == -1 || thread_table[i].estimated_time < shortest_time)) {
            shortest_time = thread_table[i].estimated_time;
            next = i;
        }
    }

    return next;
}

// First Come First Served scheduler
int scheduler_fcfs() {
    struct timespec earliest = {0};
    int next = current_thread;
    int found = 0;

    for (int i = 0; i < thread_count; i++) {
        if (thread_table[i].state == READY) {
            if (!found ||
                (thread_table[i].creation_time.tv_sec < earliest.tv_sec) ||
                (thread_table[i].creation_time.tv_sec == earliest.tv_sec &&
                 thread_table[i].creation_time.tv_nsec < earliest.tv_nsec)) {
                earliest = thread_table[i].creation_time;
                next = i;
                found = 1;
            }
        }
    }

    return next;
}

// Real-time scheduler (Earliest Deadline First)
int scheduler_rt() {
    int earliest_deadline = -1;
    int next = current_thread;

    for (int i = 0; i < thread_count; i++) {
        if (thread_table[i].state == READY &&
            (earliest_deadline == -1 || thread_table[i].deadline < earliest_deadline)) {
            earliest_deadline = thread_table[i].deadline;
            next = i;
        }
    }

    return next;
}

// Select next thread based on scheduler algorithm
int select_next_thread() {
    switch (scheduler_algo) {
        case SCHEDULER_RR:
            return scheduler_rr();
        case SCHEDULER_PRIO:
            return scheduler_priority();
        case SCHEDULER_SJF:
            return scheduler_sjf();
        case SCHEDULER_FCFS:
            return scheduler_fcfs();
        case SCHEDULER_RT:
            return scheduler_rt();
        default:
            return scheduler_rr();
    }
}

// Scheduler function
void scheduler() {
    while (1) {
        // Select next thread
        int next = select_next_thread();

        // If no thread is ready, return
        if (next < 0) {
            return;
        }

        // Switch to selected thread
        if (next != current_thread) {
            int prev = current_thread;
            current_thread = next;
            thread_table[current_thread].state = RUNNING;
            swapcontext(&scheduler_context, &thread_table[current_thread].context);
        }
    }
}

// Set scheduler algorithm
void CEthread_set_scheduler(scheduler_type_t algo) {
    scheduler_algo = algo;
}

// Yield current thread
void CEthread_yield() {
    init_threads();

    // Save current context and switch to scheduler
    swapcontext(&thread_table[current_thread].context, &scheduler_context);
}

// ---------------- Enhanced Road Crossing Simulation ----------------

// Direction and car types
typedef enum { LEFT = 0, RIGHT = 1 } Direction;
typedef enum { NORMAL = 0, SPORT = 1, EMERGENCY = 2 } CarType;

// Car structure
typedef struct {
    int id;
    Direction dir;
    CarType type;
    int priority;          // Priority for scheduling
    int estimated_time;    // For SJF scheduling
    int deadline;          // For real-time scheduling (emergency vehicles)
} Car;

// Mutex for road access
cemutex_t road_mutex;

// Configuration
char flow_method[16];      // "FIFO", "EQUITY" or "SIGNAL"
int road_length;
int base_speed;
int num_left, num_right;
int W;
int signal_time;
int max_wait_emergency;
int remaining_left, remaining_right;

// State for EQUITY and SIGNAL
Direction current_dir;
int cars_in_window;

// Function to get car speed based on type
int get_speed(CarType type) {
    switch(type) {
        case NORMAL:    return base_speed;
        case SPORT:     return base_speed * 2;
        case EMERGENCY: return base_speed * 3;
    }
    return base_speed;
}

// Get name for car type
const char* type_name(CarType t) {
    return t == NORMAL    ? "NORMAL"  \
         : t == SPORT     ? "SPORT"   \
         : /* EMERGENCY */  "EMERGENCY";
}

// Calculate elapsed time in seconds
int elapsed_sec(struct timespec then) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int)(now.tv_sec - then.tv_sec);
}

// Signal thread function to change direction periodically
void* signal_thread(void* arg) {
    while (1) {
        sleep(signal_time);
        CEmutex_lock(&road_mutex);
        current_dir = (current_dir == LEFT) ? RIGHT : LEFT;
        cars_in_window = 0;
        printf("[Signal] Changed direction: %s\n",
               current_dir == LEFT ? "LEFT" : "RIGHT");
        CEmutex_unlock(&road_mutex);
    }
    return NULL;
}

// Car thread function
void* car_thread(void* arg) {
    Car* car = (Car*)arg;
    struct timespec arrival;
    clock_gettime(CLOCK_REALTIME, &arrival);

    int speed = get_speed(car->type);
    long travel_time_us = (road_length * 1000000L) / speed;

    printf("[Arrive] Car %d [%s] from %s side.\n",
           car->id, type_name(car->type),
           car->dir == LEFT ? "LEFT" : "RIGHT");

    CEmutex_lock(&road_mutex);

    // Emergency vehicles get special treatment
    if (car->type == EMERGENCY) {
        // Check if we're already past deadline
        if (elapsed_sec(arrival) >= max_wait_emergency) {
            printf("[Timeout] Car %d [EMERGENCY] already past deadline, forcing entry\n", car->id);
            goto enter_road;
        }

        // Calculate absolute deadline
        struct timespec deadline = {
            .tv_sec = arrival.tv_sec + max_wait_emergency,
            .tv_nsec = arrival.tv_nsec
        };

        // Wait with deadline awareness
        while (1) {
            // Check if we can enter now based on flow method
            int can_enter = 0;

            if (strcmp(flow_method, "FIFO") == 0) {
                can_enter = 1; // FIFO is simple - first come first served
            }
            else if (strcmp(flow_method, "EQUITY") == 0) {
                can_enter = (car->dir == current_dir && cars_in_window < W) ||
                           (remaining_left == 0 && car->dir == RIGHT) ||
                           (remaining_right == 0 && car->dir == LEFT);
            }
            else if (strcmp(flow_method, "SIGNAL") == 0) {
                can_enter = (car->dir == current_dir);
            }

            if (can_enter) break;

            // Check if we've reached our deadline
            if (elapsed_sec(arrival) >= max_wait_emergency) {
                printf("[Timeout] Car %d [EMERGENCY] exceeded wait time (%d s), forcing entry\n",
                       car->id, max_wait_emergency);
                break;
            }

            // Temporarily unlock to allow other vehicles to proceed
            // This prevents deadlock when an emergency vehicle is waiting
            CEmutex_unlock(&road_mutex);
            usleep(100000); // Sleep for 100ms
            CEmutex_lock(&road_mutex);

            // Check again if we've reached our deadline
            if (elapsed_sec(arrival) >= max_wait_emergency) {
                printf("[Timeout] Car %d [EMERGENCY] reached deadline, forcing entry\n", car->id);
                break;
            }
        }
    }
    else {
        // Normal waiting logic for non-emergency vehicles
        if (strcmp(flow_method, "EQUITY") == 0) {
            while (car->dir != current_dir || cars_in_window >= W) {
                if ((current_dir == LEFT && remaining_left == 0) ||
                    (current_dir == RIGHT && remaining_right == 0)) {
                    cars_in_window = 0;
                    current_dir = car->dir;
                } else {
                    // Temporarily unlock to allow other vehicles to proceed
                    CEmutex_unlock(&road_mutex);
                    usleep(100000); // Sleep for 100ms
                    CEmutex_lock(&road_mutex);
                }
            }
        }
        else if (strcmp(flow_method, "SIGNAL") == 0) {
            while (car->dir != current_dir) {
                // Temporarily unlock to allow other vehicles to proceed
                CEmutex_unlock(&road_mutex);
                usleep(100000); // Sleep for 100ms
                CEmutex_lock(&road_mutex);
            }
        }
        // FIFO: no waiting needed beyond mutex acquisition
    }

enter_road:
    printf("[Enter ] Car %d [%s] from %s side.\n",
           car->id, type_name(car->type),
           car->dir == LEFT ? "LEFT" : "RIGHT");

    // Unlock mutex while simulating road crossing
    CEmutex_unlock(&road_mutex);

    // Simulate crossing the road
    usleep(travel_time_us);

    // Lock mutex to update state
    CEmutex_lock(&road_mutex);

    printf("[Exit  ] Car %d [%s] from %s side.\n",
           car->id, type_name(car->type),
           car->dir == LEFT ? "LEFT" : "RIGHT");

    // Update state
    if (strcmp(flow_method, "EQUITY") == 0) {
        cars_in_window++;
        if (car->dir == LEFT) remaining_left--;
        else remaining_right--;

        if (cars_in_window >= W ||
            (current_dir == LEFT && remaining_left == 0) ||
            (current_dir == RIGHT && remaining_right == 0)) {
            cars_in_window = 0;
            current_dir = (current_dir == LEFT) ? RIGHT : LEFT;
        }
    }
    else if (strcmp(flow_method, "SIGNAL") == 0) {
        if (car->dir == LEFT) remaining_left--;
        else remaining_right--;
    }
    else { // FIFO
        if (car->dir == LEFT) remaining_left--;
        else remaining_right--;
    }

    CEmutex_unlock(&road_mutex);
    free(car);
    return NULL;
}

// Create and spawn cars
void spawn_cars(Direction side, CarType type, int count, int* id) {
    for (int i = 0; i < count; ++i) {
        Car* c = malloc(sizeof(Car));
        c->id = ++(*id);
        c->dir = side;
        c->type = type;

        // Set scheduling parameters based on car type
        switch(type) {
            case NORMAL:
                c->priority = 1;
                c->estimated_time = road_length / base_speed;
                c->deadline = 0; // No hard deadline
                break;
            case SPORT:
                c->priority = 2;
                c->estimated_time = road_length / (base_speed * 2);
                c->deadline = 0; // No hard deadline
                break;
            case EMERGENCY:
                c->priority = 10; // Highest priority
                c->estimated_time = road_length / (base_speed * 3);
                c->deadline = max_wait_emergency; // Hard deadline
                break;
        }

        // Create thread with appropriate scheduler parameters
        cethread_t thread;
        CEthread_create(&thread, car_thread, c, c->priority, c->estimated_time, c->deadline);
    }
}

// Main function
int main() {
    // Initialize the CEThreads system
    CEthread_set_scheduler(SCHEDULER_RT); // Use real-time scheduling for emergency vehicles

    printf("Road Crossing Simulation using CEThreads\n");

    // Read configuration
    FILE* fp = fopen("config.txt", "r");
    if (!fp) {
        // Create a default config if file doesn't exist
        fp = fopen("config.txt", "w");
        if (!fp) {
            perror("Failed to create config.txt");
            return 1;
        }

        fprintf(fp, "flow_method=EQUITY\n");
        fprintf(fp, "road_length=100\n");
        fprintf(fp, "car_speed=10\n");
        fprintf(fp, "num_left=5\n");
        fprintf(fp, "num_right=5\n");
        fprintf(fp, "W=3\n");
        fprintf(fp, "signal_time=5\n");
        fprintf(fp, "max_wait_emergency=3\n");
        fprintf(fp, "normales_left=2\n");
        fprintf(fp, "deportivos_left=2\n");
        fprintf(fp, "emergencia_left=1\n");
        fprintf(fp, "normales_right=2\n");
        fprintf(fp, "deportivos_right=2\n");
        fprintf(fp, "emergencia_right=1\n");

        fclose(fp);
        fp = fopen("config.txt", "r");
        if (!fp) {
            perror("Failed to open config.txt");
            return 1;
        }
    }

    char key[32], val[32];
    int normales_left = 0, deportivos_left = 0, emergencia_left = 0;
    int normales_right = 0, deportivos_right = 0, emergencia_right = 0;

    while (fscanf(fp, "%31[^=]=%31s\n", key, val) == 2) {
        if      (!strcmp(key, "flow_method"))      strcpy(flow_method, val);
        else if (!strcmp(key, "road_length"))      road_length = atoi(val);
        else if (!strcmp(key, "car_speed"))        base_speed = atoi(val);
        else if (!strcmp(key, "num_left"))         num_left = atoi(val);
        else if (!strcmp(key, "num_right"))        num_right = atoi(val);
        else if (!strcmp(key, "W"))                W = atoi(val);
        else if (!strcmp(key, "signal_time"))      signal_time = atoi(val);
        else if (!strcmp(key, "max_wait_emergency")) max_wait_emergency = atoi(val);
        else if (!strcmp(key, "normales_left"))    normales_left = atoi(val);
        else if (!strcmp(key, "deportivos_left"))  deportivos_left = atoi(val);
        else if (!strcmp(key, "emergencia_left"))  emergencia_left = atoi(val);
        else if (!strcmp(key, "normales_right"))   normales_right = atoi(val);
        else if (!strcmp(key, "deportivos_right")) deportivos_right = atoi(val);
        else if (!strcmp(key, "emergencia_right")) emergencia_right = atoi(val);
    }
    fclose(fp);

    printf("Configuration loaded:\n");
    printf("- Flow method: %s\n", flow_method);
    printf("- Road length: %d\n", road_length);
    printf("- Base speed: %d\n", base_speed);
    printf("- Max wait for emergency vehicles: %d seconds\n", max_wait_emergency);

    // Initialize synchronization and state
    CEmutex_init(&road_mutex);
    remaining_left  = normales_left + deportivos_left + emergencia_left;
    remaining_right = normales_right + deportivos_right + emergencia_right;
    cars_in_window  = 0;
    current_dir     = LEFT;

    // Launch signal thread if needed
    cethread_t signal_tid;
    if (!strcmp(flow_method, "SIGNAL")) {
        CEthread_create(&signal_tid, signal_thread, NULL, 10, 0, 0);
    }

    // Create cars
    int id = 0;

    printf("Spawning vehicles...\n");
    spawn_cars(LEFT, NORMAL, normales_left, &id);
    spawn_cars(LEFT, SPORT, deportivos_left, &id);
    spawn_cars(LEFT, EMERGENCY, emergencia_left, &id);
    spawn_cars(RIGHT, NORMAL, normales_right, &id);
    spawn_cars(RIGHT, SPORT, deportivos_right, &id);
    spawn_cars(RIGHT, EMERGENCY, emergencia_right, &id);

    // Main thread will now act as the scheduler
    printf("Starting simulation...\n");

    // Run until all cars have crossed
    while (remaining_left > 0 || remaining_right > 0) {
        CEthread_yield(); // Yield to other threads
        usleep(10000);    // Sleep a bit to avoid busy waiting
    }

    // Clean up
    CEmutex_destroy(&road_mutex);

    printf("Simulation complete. All vehicles have crossed.\n");
    return 0;
}