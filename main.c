#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "CEThreads.h"  // Replace pthread.h with CEThreads.h

// Simulación de cruce de una sola vía con modos FIFO, EQUITY y LETRERO (SIGNAL)
// Parámetros leídos desde archivo "config.txt"
// Emergencia: vehículo real-time hard, no puede esperar más que max_wait_emergency (s)

typedef enum { LEFT = 0, RIGHT = 1 } Direction;
typedef enum { NORMAL = 0, SPORT = 1, EMERGENCY = 2 } CarType;

typedef struct {
    int id;
    Direction dir;
    CarType type;
    struct timespec arrival_time;
    int priority;           // For priority scheduler
    int estimated_time;     // For SJF scheduler
    int deadline;           // For real-time scheduler
} Car;

CEmutex_t road_mutex;      // Replace pthread_mutex_t
CEcond_t road_cond;        // Replace pthread_cond_t
CEmutex_t queue_mutex;     // Replace pthread_mutex_t

// Car queue implementation for ordered priority queue
typedef struct CarQueueNode {
    Car* car;
    int priority;
    struct CarQueueNode* next;
} CarQueueNode;

// Extended car queue node to include scheduler data
typedef struct {
    CarQueueNode* head;
    CarQueueNode* tail;
    int size;                      // Track queue size
    CEmutex_t mutex;               // Replace pthread_mutex_t
} CarQueue;

CarQueue left_queue;
CarQueue right_queue;

// Configuración
char flow_method[16];      // "FIFO", "EQUITY" o "SIGNAL"
int road_length;
int base_speed;
int num_left, num_right;
int W;
int signal_time;
int max_wait_emergency;
int normales_left, deportivos_left, emergencia_left;
int normales_right, deportivos_right, emergencia_right;

// Estado para EQUITY y SIGNAL
Direction current_dir;
int cars_in_window;
int remaining_left, remaining_right;
int emergency_vehicles_waiting_left = 0;
int emergency_vehicles_waiting_right = 0;

// Variable de control para saber cuántos vehículos hay en la carretera
// y desde qué dirección vienen
Direction road_occupied_dir = LEFT; // Default direction, will be updated
int cars_on_road = 0;
int cars_on_road_left = 0;
int cars_on_road_right = 0;

// Scheduler types
typedef enum {
    FCFS = 0,    // First Come First Served
    RR = 1,      // Round Robin
    PRIORITY = 2, // Priority scheduling
    SJF = 3,     // Shortest Job First
    REALTIME = 4 // Real-time scheduling
} SchedulerType;

// Additional variables for scheduling
char scheduler_method[16] = "FCFS";  // Default scheduler
SchedulerType current_scheduler = FCFS;
int time_quantum = 2;               // For RR scheduling, time in seconds
int default_priority = 5;           // Default priority level (1-10)
int default_estimated_time = 5;     // Default estimated time (seconds)
int time_slice_remaining = 0;       // Time slice remaining for current car in RR

// Get next car from queue
Car* dequeue_car(CarQueue* queue) {
    CEmutex_lock(&queue_mutex);  // Replace pthread_mutex_lock

    if (queue->head == NULL) {
        CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock
        return NULL;
    }

    // Get first car
    CarQueueNode* node = queue->head;
    Car* car = node->car;

    // Update queue
    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }

    // Update emergency vehicle counter if needed
    if (car->type == EMERGENCY) {
        if (car->dir == LEFT) {
            emergency_vehicles_waiting_left--;
        } else {
            emergency_vehicles_waiting_right--;
        }
    }

    // Free node
    free(node);

    CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock
    return car;
}

// Check if there are any emergency vehicles in the queue
// whose wait time is approaching the deadline
int check_emergency_deadlines(CarQueue* queue) {
    CEmutex_lock(&queue_mutex);  // Replace pthread_mutex_lock

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    // No emergency vehicles waiting
    if ((queue == &left_queue && emergency_vehicles_waiting_left == 0) ||
        (queue == &right_queue && emergency_vehicles_waiting_right == 0)) {
        CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock
        return 0;
    }

    CarQueueNode* node = queue->head;

    while (node != NULL) {
        if (node->car->type == EMERGENCY) {
            long wait_time = (now.tv_sec - node->car->arrival_time.tv_sec);

            // If emergency vehicle is approaching deadline (80% of max wait)
            if (wait_time >= (max_wait_emergency * 0.8)) {
                CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock
                return 1;
            }
        }
        node = node->next;
    }

    CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock
    return 0;
}

// Signal thread to change traffic direction
void* signal_thread(void* arg) {
    while (1) {
        sleep(signal_time);
        CEmutex_lock(&road_mutex);  // Replace pthread_mutex_lock
        // Only change direction if there are no emergency vehicles approaching deadline
        // in the current direction
        if ((current_dir == LEFT && !check_emergency_deadlines(&left_queue)) ||
            (current_dir == RIGHT && !check_emergency_deadlines(&right_queue))) {
            current_dir = (current_dir == LEFT) ? RIGHT : LEFT;
            cars_in_window = 0;
            printf("[Signal] Cambio de sentido: %s\n",
                current_dir == LEFT ? "LEFT" : "RIGHT");
            CEcond_broadcast(&road_cond);  // Replace pthread_cond_broadcast
        } else {
            printf("[Signal] Maintaining direction due to emergency vehicle priority: %s\n",
                current_dir == LEFT ? "LEFT" : "RIGHT");
        }
        CEmutex_unlock(&road_mutex);  // Replace pthread_mutex_unlock
    }
    return NULL;
}

long get_speed(CarType type) {
    switch(type) {
        case NORMAL:    return base_speed;
        case SPORT:     return base_speed * 2;
        case EMERGENCY: return base_speed * 3;
    }
    return base_speed;
}

const char* type_name(CarType t) {
    return t == NORMAL    ? "NORMAL"  \
         : t == SPORT     ? "SPORT"   \
         : /* EMERGENCY */  "EMERGENCY";
}

// Calculate elapsed time in seconds between now and then
int elapsed_sec(struct timespec then) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int)(now.tv_sec - then.tv_sec);
}

// Calculate remaining time to deadline
int time_to_deadline(struct timespec arrival) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int elapsed = (int)(now.tv_sec - arrival.tv_sec);
    return max_wait_emergency - elapsed;
}

int can_enter_road(Car* car) {
    // If road is empty, car can enter
    if (cars_on_road == 0) {
        return 1;
    }

    // If road has cars going in the same direction, car can enter
    // But ONLY if there are NO cars going in the opposite direction
    if (car->dir == LEFT && cars_on_road_left > 0 && cars_on_road_right == 0) {
        return 1;
    }
    if (car->dir == RIGHT && cars_on_road_right > 0 && cars_on_road_left == 0) {
        return 1;
    }

    // Otherwise, car cannot enter (road is occupied by cars going in opposite direction)
    return 0;
}

// Function to select scheduler type from string
SchedulerType get_scheduler_type(const char* method) {
    if (strcmp(method, "RR") == 0) return RR;
    if (strcmp(method, "PRIORITY") == 0) return PRIORITY;
    if (strcmp(method, "SJF") == 0) return SJF;
    if (strcmp(method, "REALTIME") == 0) return REALTIME;
    return FCFS; // Default
}

// Requeue car for Round Robin scheduling
void requeue_car(CarQueue* queue, Car* car) {
    // Only used for RR scheduler
    if (current_scheduler != RR) return;

    CEmutex_lock(&queue_mutex);  // Replace pthread_mutex_lock

    // Create new node
    CarQueueNode* new_node = (CarQueueNode*)malloc(sizeof(CarQueueNode));
    new_node->car = car;
    new_node->next = NULL;
    new_node->priority = (car->type == EMERGENCY) ? 10 :
                         (car->type == SPORT) ? 5 : 1;

    // Add to end of queue
    if (queue->head == NULL) {
        queue->head = new_node;
        queue->tail = new_node;
    } else {
        queue->tail->next = new_node;
        queue->tail = new_node;
    }

    queue->size++;
    CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock

    // Signal waiting threads
    CEcond_broadcast(&road_cond);  // Replace pthread_cond_broadcast
}

// Read scheduler configuration from file
void read_scheduler_config() {
    FILE* fp = fopen("scheduler.txt", "r");
    if (!fp) {
        // Create a default scheduler config if file doesn't exist
        fp = fopen("scheduler.txt", "w");
        if (!fp) {
            perror("Failed to create scheduler.txt");
            return;
        }

        fprintf(fp, "scheduler_method=FCFS\n");
        fprintf(fp, "time_quantum=2\n");
        fprintf(fp, "default_priority=5\n");
        fprintf(fp, "default_estimated_time=5\n");

        fclose(fp);
        fp = fopen("scheduler.txt", "r");
        if (!fp) {
            perror("Failed to open scheduler.txt");
            return;
        }
    }

    char key[32], val[32];
    while (fscanf(fp, "%31[^=]=%31s\n", key, val) == 2) {
        if      (!strcmp(key, "scheduler_method"))     strcpy(scheduler_method, val);
        else if (!strcmp(key, "time_quantum"))         time_quantum = atoi(val);
        else if (!strcmp(key, "default_priority"))     default_priority = atoi(val);
        else if (!strcmp(key, "default_estimated_time")) default_estimated_time = atoi(val);
    }
    fclose(fp);

    // Set current scheduler based on config
    current_scheduler = get_scheduler_type(scheduler_method);

    printf("Scheduler configuration loaded:\n");
    printf("- Scheduler method: %s\n", scheduler_method);
    printf("- Time quantum (for RR): %d seconds\n", time_quantum);
    printf("- Default priority: %d\n", default_priority);
    printf("- Default estimated time: %d seconds\n", default_estimated_time);
}

// Initialize car queue with mutex
void init_queue(CarQueue* queue) {
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
    CEmutex_init(&queue->mutex, NULL);  // Replace pthread_mutex_init
}

// Queue management functions with scheduler support
void enqueue_car(CarQueue* queue, Car* car) {
    CEmutex_lock(&queue_mutex);  // Replace pthread_mutex_lock

    // Create new node
    CarQueueNode* new_node = (CarQueueNode*)malloc(sizeof(CarQueueNode));
    new_node->car = car;
    new_node->next = NULL;

    // Set base priority based on car type
    if (car->type == EMERGENCY) {
        new_node->priority = 10;
        if (car->dir == LEFT) {
            emergency_vehicles_waiting_left++;
        } else {
            emergency_vehicles_waiting_right++;
        }
    }
    else if (car->type == SPORT) {
        new_node->priority = 5;
    }
    else { // NORMAL
        new_node->priority = 1;
    }

    // If queue is empty
    if (queue->head == NULL) {
        queue->head = new_node;
        queue->tail = new_node;
        queue->size++;
        CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock
        return;
    }

    // Apply current scheduling algorithm
    switch (current_scheduler) {
        case FCFS:
            // FCFS: Insert at end (already default behavior)
            queue->tail->next = new_node;
            queue->tail = new_node;
            break;

        case PRIORITY:
            // Priority: Higher priority first
            {
                CarQueueNode *current = queue->head;
                CarQueueNode *prev = NULL;

                // Find position based on priority
                while (current != NULL && current->priority >= new_node->priority) {
                    prev = current;
                    current = current->next;
                }

                // Insert at beginning
                if (prev == NULL) {
                    new_node->next = queue->head;
                    queue->head = new_node;
                }
                // Insert at end
                else if (current == NULL) {
                    prev->next = new_node;
                    queue->tail = new_node;
                }
                // Insert in middle
                else {
                    prev->next = new_node;
                    new_node->next = current;
                }
            }
            break;

        case SJF:
            // SJF: Shortest estimated time first
            {
                // Calculate estimated time based on car type and road length
                int estimated_time;
                switch(car->type) {
                    case NORMAL:    estimated_time = road_length / base_speed; break;
                    case SPORT:     estimated_time = road_length / (base_speed * 2); break;
                    case EMERGENCY: estimated_time = road_length / (base_speed * 3); break;
                    default:        estimated_time = road_length / base_speed;
                }

                // Store estimated time in the car structure
                car->estimated_time = estimated_time;

                CarQueueNode *current = queue->head;
                CarQueueNode *prev = NULL;

                // Find position based on estimated time (shorter first)
                while (current != NULL) {
                    int current_estimated_time = current->car->estimated_time;

                    if (estimated_time < current_estimated_time) break;

                    prev = current;
                    current = current->next;
                }

                // Insert at beginning
                if (prev == NULL) {
                    new_node->next = queue->head;
                    queue->head = new_node;
                }
                // Insert at end
                else if (current == NULL) {
                    prev->next = new_node;
                    queue->tail = new_node;
                }
                // Insert in middle
                else {
                    prev->next = new_node;
                    new_node->next = current;
                }
            }
            break;

        case REALTIME:
            // Real-time: Emergency vehicles first, then by deadline
            {
                // Set deadline for emergency vehicles
                if (car->type == EMERGENCY) {
                    car->deadline = max_wait_emergency;

                    // Emergency vehicles always at front
                    // But ordered by arrival time among themselves
                    CarQueueNode *current = queue->head;
                    CarQueueNode *prev = NULL;

                    while (current != NULL && current->car->type == EMERGENCY) {
                        prev = current;
                        current = current->next;
                    }

                    if (prev == NULL) {
                        // First emergency vehicle
                        new_node->next = queue->head;
                        queue->head = new_node;
                    } else {
                        // Insert after last emergency vehicle
                        new_node->next = current;
                        prev->next = new_node;
                        if (current == NULL) {
                            queue->tail = new_node;
                        }
                    }
                } else {
                    // Non-emergency vehicles at end
                    queue->tail->next = new_node;
                    queue->tail = new_node;
                }
            }
            break;

        case RR:
            // Round Robin: Same as FCFS for insertion, but with time slices during execution
            queue->tail->next = new_node;
            queue->tail = new_node;
            break;

        default:
            // Default to FCFS
            queue->tail->next = new_node;
            queue->tail = new_node;
    }

    queue->size++;
    CEmutex_unlock(&queue_mutex);  // Replace pthread_mutex_unlock
}

void* car_thread(void* arg) {
    Car* car = (Car*)arg;

    // Record arrival time
    clock_gettime(CLOCK_REALTIME, &car->arrival_time);

    // Initialize car based on scheduler
    switch (current_scheduler) {
        case PRIORITY:
            // Set priority based on car type
            car->priority = (car->type == EMERGENCY) ? 10 :
                            (car->type == SPORT) ? 5 : 1;
            break;

        case SJF:
            // Calculate estimated time based on car type and road length
            car->estimated_time = road_length / get_speed(car->type);
            break;

        case REALTIME:
            // Set deadline for emergency vehicles
            if (car->type == EMERGENCY) {
                car->deadline = max_wait_emergency;
            }
            break;

        default:
            // No special initialization for FCFS and RR
            break;
    }

    long speed = get_speed(car->type);
    long travel_time_us = (road_length * 1000000L) / speed;

    printf("[Arrive] Car %d [%s] from %s side\n",
        car->id,
        type_name(car->type),
        car->dir == LEFT ? "LEFT" : "RIGHT");

    // Add car to appropriate queue
    if (car->dir == LEFT) {
        enqueue_car(&left_queue, car);
    } else {
        enqueue_car(&right_queue, car);
    }

    CEmutex_lock(&road_mutex);

    // Special handling for Round Robin
    if (current_scheduler == RR && car->type != EMERGENCY) {
        // Use current time_slice_remaining for RR scheduling
        // But this doesn't apply to emergency vehicles - they still get priority
        while (1) {
            // Check if car is at front of its queue
            CEmutex_lock(&queue_mutex);
            int is_front = (car->dir == LEFT) ?
                (left_queue.head && left_queue.head->car->id == car->id) :
                (right_queue.head && right_queue.head->car->id == car->id);
            CEmutex_unlock(&queue_mutex);

            // CRITICAL CHANGE: This is where we check if the car can enter the road
            // We must ensure no cars from opposite direction are on the road
            if (is_front && can_enter_road(car)) {
                // Car can enter the road
                if (car->dir == LEFT) {
                    Car* next_car = dequeue_car(&left_queue);
                    if (next_car->id != car->id) {
                        printf("ERROR: Queue mismatch for car %d!\n", car->id);
                    }
                } else {
                    Car* next_car = dequeue_car(&right_queue);
                    if (next_car->id != car->id) {
                        printf("ERROR: Queue mismatch for car %d!\n", car->id);
                    }
                }
                break;
            }

            // Check if any emergency vehicle is approaching deadline
            int emergency_pending = check_emergency_deadlines(&left_queue) ||
                                   check_emergency_deadlines(&right_queue);

            if (emergency_pending) {
                CEcond_wait(&road_cond, &road_mutex);
                continue;
            }

            // Wait with timeout - CEThreads doesn't have built-in timedwait so we'll adapt
            struct timespec wait_time = {
                .tv_sec = time(NULL) + 0,
                .tv_nsec = 100000000 // 100ms
            };
            CEcond_timedwait(&road_cond, &road_mutex, &wait_time);
        }
    }
    // Emergency vehicle handling with strict deadline enforcement
    else if (car->type == EMERGENCY) {
        while (1) {
            // Check if deadline is approaching
            int remaining = time_to_deadline(car->arrival_time);

            // CRITICAL CHANGE: Even emergency vehicles must respect opposite direction traffic
            // unless their deadline is imminent
            if (remaining <= 1) {
                printf("[EMERGENCY OVERRIDE] Car %d forcing entry with %d seconds remaining to deadline\n",
                       car->id, remaining);

                if (car->dir == LEFT) {
                    Car* next_car = dequeue_car(&left_queue);
                    if (next_car->id != car->id) {
                        printf("ERROR: Queue mismatch for car %d!\n", car->id);
                    }
                } else {
                    Car* next_car = dequeue_car(&right_queue);
                    if (next_car->id != car->id) {
                        printf("ERROR: Queue mismatch for car %d!\n", car->id);
                    }
                }
                break;
            }
            // If emergency vehicle can enter and road is clear in opposite direction, proceed
            else if (can_enter_road(car)) {
                if (car->dir == LEFT) {
                    Car* next_car = dequeue_car(&left_queue);
                    if (next_car->id != car->id) {
                        printf("ERROR: Queue mismatch for car %d!\n", car->id);
                    }
                } else {
                    Car* next_car = dequeue_car(&right_queue);
                    if (next_car->id != car->id) {
                        printf("ERROR: Queue mismatch for car %d!\n", car->id);
                    }
                }
                break;
            }

            // Wait with timeout for road to become available
            struct timespec wait_time = {
                .tv_sec = time(NULL) + 0,
                .tv_nsec = 100000000 // 100ms
            };
            CEcond_timedwait(&road_cond, &road_mutex, &wait_time);
        }
    }
    // For priority-based schedulers (Priority, SJF, REALTIME) and FCFS
    else {
        // Regular car waiting logic based on flow control method
        if (strcmp(flow_method, "FIFO") == 0) {
            // FIFO: Wait until car is at front of its queue AND can enter road
            while (1) {
                CEmutex_lock(&queue_mutex);
                int is_front = (car->dir == LEFT) ?
                    (left_queue.head && left_queue.head->car->id == car->id) :
                    (right_queue.head && right_queue.head->car->id == car->id);
                CEmutex_unlock(&queue_mutex);

                // CRITICAL CHANGE: Enforce can_enter_road check
                if (is_front && can_enter_road(car)) {
                    // Remove car from queue when it enters
                    if (car->dir == LEFT) {
                        Car* next_car = dequeue_car(&left_queue);
                        if (next_car->id != car->id) {
                            printf("ERROR: Queue mismatch for car %d!\n", car->id);
                        }
                    } else {
                        Car* next_car = dequeue_car(&right_queue);
                        if (next_car->id != car->id) {
                            printf("ERROR: Queue mismatch for car %d!\n", car->id);
                        }
                    }
                    break;
                }

                // Check if any emergency vehicle is approaching deadline
                int emergency_pending = check_emergency_deadlines(&left_queue) ||
                                       check_emergency_deadlines(&right_queue);

                // If there's an emergency vehicle approaching deadline, yield and wait
                if (emergency_pending) {
                    CEcond_wait(&road_cond, &road_mutex);
                    continue;
                }

                // Wait with timeout
                struct timespec wait_time = {
                    .tv_sec = time(NULL) + 0,
                    .tv_nsec = 100000000 // 100ms
                };
                CEcond_timedwait(&road_cond, &road_mutex, &wait_time);
            }
        }
        else if (strcmp(flow_method, "EQUITY") == 0) {
            // EQUITY: Wait until car's direction is allowed and it's within the window
            while (1) {
                CEmutex_lock(&queue_mutex);
                int is_front = (car->dir == LEFT) ?
                    (left_queue.head && left_queue.head->car->id == car->id) :
                    (right_queue.head && right_queue.head->car->id == car->id);
                CEmutex_unlock(&queue_mutex);

                int can_go = (car->dir == current_dir && cars_in_window < W && is_front) ||
                            (current_dir == LEFT && remaining_left == 0 && car->dir == RIGHT && is_front) ||
                            (current_dir == RIGHT && remaining_right == 0 && car->dir == LEFT && is_front);

                // CRITICAL CHANGE: Add can_enter_road check
                if (can_go && can_enter_road(car)) {
                    // Remove car from queue when it enters
                    if (car->dir == LEFT) {
                        Car* next_car = dequeue_car(&left_queue);
                        if (next_car->id != car->id) {
                            printf("ERROR: Queue mismatch for car %d!\n", car->id);
                        }
                    } else {
                        Car* next_car = dequeue_car(&right_queue);
                        if (next_car->id != car->id) {
                            printf("ERROR: Queue mismatch for car %d!\n", car->id);
                        }
                    }
                    break;
                }

                // Check if any emergency vehicle is approaching deadline
                int emergency_pending = check_emergency_deadlines(&left_queue) ||
                                       check_emergency_deadlines(&right_queue);

                // If there's an emergency vehicle approaching deadline, yield and wait
                if (emergency_pending) {
                    CEcond_wait(&road_cond, &road_mutex);
                    continue;
                }

                // Wait with timeout
                struct timespec wait_time = {
                    .tv_sec = time(NULL) + 0,
                    .tv_nsec = 100000000 // 100ms
                };
                CEcond_timedwait(&road_cond, &road_mutex, &wait_time);
            }
        }
        else if (strcmp(flow_method, "SIGNAL") == 0) {
            // SIGNAL: Wait until car's direction is allowed
            while (1) {
                CEmutex_lock(&queue_mutex);
                int is_front = (car->dir == LEFT) ?
                    (left_queue.head && left_queue.head->car->id == car->id) :
                    (right_queue.head && right_queue.head->car->id == car->id);
                CEmutex_unlock(&queue_mutex);

                // CRITICAL CHANGE: Add can_enter_road check
                if (car->dir == current_dir && is_front && can_enter_road(car)) {
                    // Remove car from queue when it enters
                    if (car->dir == LEFT) {
                        Car* next_car = dequeue_car(&left_queue);
                        if (next_car->id != car->id) {
                            printf("ERROR: Queue mismatch for car %d!\n", car->id);
                        }
                    } else {
                        Car* next_car = dequeue_car(&right_queue);
                        if (next_car->id != car->id) {
                            printf("ERROR: Queue mismatch for car %d!\n", car->id);
                        }
                    }
                    break;
                }

                // Check if any emergency vehicle is approaching deadline
                int emergency_pending = check_emergency_deadlines(&left_queue) ||
                                       check_emergency_deadlines(&right_queue);

                // If there's an emergency vehicle approaching deadline, yield and wait
                if (emergency_pending) {
                    CEcond_wait(&road_cond, &road_mutex);
                    continue;
                }

                // Wait with timeout
                struct timespec wait_time = {
                    .tv_sec = time(NULL) + 0,
                    .tv_nsec = 100000000 // 100ms
                };
                CEcond_timedwait(&road_cond, &road_mutex, &wait_time);
            }
        }
    }

    // IMPORTANT: Update road occupation BEFORE the car enters the road
    cars_on_road++;
    if (car->dir == LEFT) {
        cars_on_road_left++;
    } else {
        cars_on_road_right++;
    }
    road_occupied_dir = car->dir;

    // Actual crossing
    printf("[Enter ] Car %d [%s] from %s side (Scheduler: %s). Total cars on road: LEFT=%d, RIGHT=%d\n",
           car->id, type_name(car->type),
           car->dir == LEFT ? "LEFT" : "RIGHT", scheduler_method,
           cars_on_road_left, cars_on_road_right);

    // For Round Robin, set time slice remaining
    int rr_timeout = 0;
    if (current_scheduler == RR && car->type != EMERGENCY) {
        time_slice_remaining = time_quantum;
    }

    // Release mutex while crossing to allow other cars to queue up
    CEmutex_unlock(&road_mutex);

    // For Round Robin, check if time slice expires during travel
    if (current_scheduler == RR && car->type != EMERGENCY) {
        // Calculate how long the car will take to cross
        long travel_time_seconds = travel_time_us / 1000000L;

        // Check if car will complete crossing within time slice
        if (travel_time_seconds <= time_slice_remaining) {
            // Car completes crossing within time slice
            usleep(travel_time_us);
        } else {
            // Car's time slice expires during crossing
            // Let it continue anyway since it's already on the road
            // But record that it exceeded its time slice
            printf("[RR] Car %d exceeded time slice but continuing to cross.\n", car->id);
            usleep(travel_time_us);
            rr_timeout = 1;
        }
    } else {
        // Regular crossing for other schedulers
        usleep(travel_time_us);
    }

    // Update state after crossing
    CEmutex_lock(&road_mutex);

    // Update road occupation
    cars_on_road--;
    if (car->dir == LEFT) {
        cars_on_road_left--;
        remaining_left--;
    } else {
        cars_on_road_right--;
        remaining_right--;
    }

    printf("[Exit  ] Car %d [%s] from %s side. Remaining cars on road: LEFT=%d, RIGHT=%d\n",
           car->id, type_name(car->type),
           car->dir == LEFT ? "LEFT" : "RIGHT",
           cars_on_road_left, cars_on_road_right);

    // Update flow control state
    if (strcmp(flow_method, "EQUITY") == 0) {
        cars_in_window++;

        if (cars_in_window >= W ||
            (current_dir == LEFT && remaining_left == 0) ||
            (current_dir == RIGHT && remaining_right == 0)) {
            cars_in_window = 0;
            current_dir = (current_dir == LEFT) ? RIGHT : LEFT;
            printf("[EQUITY] Changing direction to: %s\n",
                   current_dir == LEFT ? "LEFT" : "RIGHT");
        }
    }

    // For Round Robin, if car timed out during crossing, put it back in queue
    if (current_scheduler == RR && car->type != EMERGENCY && rr_timeout) {
        printf("[RR] Car %d being requeued after time slice expiration.\n", car->id);
        // Create a new car instance since this one will be freed
        Car* new_car = malloc(sizeof(Car));
        *new_car = *car;  // Copy all fields

        // Put back in appropriate queue
        if (car->dir == LEFT) {
            requeue_car(&left_queue, new_car);
        } else {
            requeue_car(&right_queue, new_car);
        }
    }

    // Notify waiting cars
    CEcond_broadcast(&road_cond);
    CEmutex_unlock(&road_mutex);

    // Free car structure - only if not requeued
    if (!(current_scheduler == RR && car->type != EMERGENCY && rr_timeout)) {
        free(car);
    }

    return NULL;
}

void spawn_cars(Direction side, CarType type, int count, int* id) {
    for (int i = 0; i < count; ++i) {
        Car* c = malloc(sizeof(Car));
        c->id = ++(*id);
        c->dir = side;
        c->type = type;

        CEthread_t tid;
        CEthread_create(&tid, NULL, car_thread, c);
        // CEThreads doesn't have detach, threads are automatically cleaned up when done
    }
}

int main() {
    printf("Road Crossing Simulation \n");

    // Initialize the CEThreads library
    CEthread_lib_init();

    // Initialize queues
    init_queue(&left_queue);
    init_queue(&right_queue);

    // Read config
    FILE* fp = fopen("config.txt", "r");
    if (!fp) {
        // Create a default config if file doesn't exist
        fp = fopen("config.txt", "w");
        if (!fp) {
            perror("Failed to create config.txt");
            return 1;
        }

        fprintf(fp, "flow_method=EQUITY\n");
        fprintf(fp, "road_length=50\n");
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

    // Read scheduler configuration
    read_scheduler_config();

    printf("Configuration loaded:\n");
    printf("- Flow method: %s\n", flow_method);
    printf("- Road length: %d\n",  road_length);
    printf("- Base speed: %d\n", base_speed);
    printf("- Max wait for emergency vehicles: %d seconds\n", max_wait_emergency);
    printf("- Scheduler method: %s\n", scheduler_method);

    // Initialize synchronization and state
    CEmutex_init(&road_mutex, NULL);
    CEcond_init(&road_cond, NULL);
    CEmutex_init(&queue_mutex, NULL);

    remaining_left  = normales_left + deportivos_left + emergencia_left;
    remaining_right = normales_right + deportivos_right + emergencia_right;
    cars_in_window  = 0;
    current_dir     = LEFT;

    // Launch signal thread if needed
    CEthread_t tidSignal;
    if (!strcmp(flow_method, "SIGNAL")) {
        CEthread_create(&tidSignal, NULL, signal_thread, NULL);
        // CEThreads doesn't have detach, threads are automatically cleaned up when done
    }

    // Create cars
    int id = 0;

    spawn_cars(LEFT, NORMAL, normales_left, &id);
    spawn_cars(LEFT, SPORT, deportivos_left, &id);
    spawn_cars(LEFT, EMERGENCY, emergencia_left, &id);
    spawn_cars(RIGHT, NORMAL, normales_right, &id);
    spawn_cars(RIGHT, SPORT, deportivos_right, &id);
    spawn_cars(RIGHT, EMERGENCY, emergencia_right, &id);

    // Wait until all cars have crossed
    while (remaining_left > 0 || remaining_right > 0) {
        usleep(100000); // Sleep 100ms to avoid busy waiting
        CEthread_yield(); // Add yield to give other threads a chance to run
    }

    // Cleanup
    CEmutex_destroy(&road_mutex);
    CEcond_destroy(&road_cond);
    CEmutex_destroy(&queue_mutex);

    // Clean up the CEThreads library
    CEthread_lib_destroy();

    printf("Simulation complete. All vehicles have crossed.\n");
    return 0;
}