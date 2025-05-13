#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include "CEThreads.h"  // Replace pthread.h with CEThreads.h

// Simulación de cruce de una sola vía con modos FIFO, EQUITY y LETRERO (SIGNAL)
// Parámetros leídos desde archivo "config.txt"
// Emergencia: vehículo real-time hard, no puede esperar más que max_wait_emergency (s)
// Window dimensions
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

// Road dimensions
#define ROAD_WIDTH 500
#define ROAD_X WINDOW_WIDTH/2 - ROAD_WIDTH/2
#define ROAD_Y (WINDOW_HEIGHT / 2 - (ROAD_HEIGHT / 2))
#define ROAD_HEIGHT 100

// Car dimensions
#define CAR_WIDTH 60
#define CAR_HEIGHT 40

gboolean simulation_running = FALSE;

gboolean update_gui(gpointer data);

static void set_cairo_color(cairo_t* cr, double r, double g, double b, double a) {
    cairo_set_source_rgba(cr, r/255.0, g/255.0, b/255.0, a/255.0);
}

void* simulation_main(void* arg);

typedef enum { LEFT = 0, RIGHT = 1 } Direction;
typedef enum { NORMAL = 0, SPORT = 1, EMERGENCY = 2 } CarType;



// Global variables to track cars for visualization
typedef struct {
    int id;
    Direction dir;
    CarType type;
    double position; // Position on the road (0-1 where 1 is completed)
    int active;      // Whether car is currently on the road
} CarVisual;

typedef struct {
    Direction dir;
    CarType type;
    int x;
    int y;
} CarDraw;

CarDraw * car_drawn;
int start_drawing_cars = 0;
GCallback spawn_cars(GtkWidget *widget, GdkEventButton event, gpointer * data);

typedef struct {
    Direction dir;
    CarType type;
    int count;  // Replace with actual type
    int * id;
} SpawnCarsParams;


#define MAX_CARS_VISUAL 50
CarVisual cars_visual[MAX_CARS_VISUAL];
int car_visual_count = 0;
CEmutex_t visual_mutex;


// Add car to visualization
void add_car_visual(int id, Direction dir, CarType type) {
    CEmutex_lock(&visual_mutex);

    // Find an empty slot or reuse
    int idx = -1;
    for (int i = 0; i < MAX_CARS_VISUAL; i++) {
        if (!cars_visual[i].active) {
            idx = i;
            break;
        }
    }

    // If no slot found, use the oldest one
    if (idx == -1) {
        idx = car_visual_count % MAX_CARS_VISUAL;
        car_visual_count++;
    }

    // Set car properties
    cars_visual[idx].id = id;
    cars_visual[idx].dir = dir;
    cars_visual[idx].type = type;
    cars_visual[idx].position = 0.0;
    cars_visual[idx].active = 1;

    CEmutex_unlock(&visual_mutex);
}

// Update car position
void update_car_visual(int id, double position) {
    CEmutex_lock(&visual_mutex);

    for (int i = 0; i < MAX_CARS_VISUAL; i++) {
        if (cars_visual[i].active && cars_visual[i].id == id) {
            cars_visual[i].position = position;
            if (position >= 1.0) {
                cars_visual[i].active = 0; // Car has exited
            }
            break;
        }
    }

    CEmutex_unlock(&visual_mutex);
}

typedef struct {
    int id;
    Direction dir;
    CarType type;
    struct timespec arrival_time;
    int priority;           // For priority scheduler
    int estimated_time;     // For SJF scheduler
    int deadline;           // For real-time scheduler
    int x;
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
int remaining_left = 0;
int remaining_right = 0;
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


GtkWidget* drawing_area;
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
    FILE* fp = fopen("/home/alexis/Documents/Tec/SO/Scheduling-Cars/scheduler.txt", "r");
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
        fp = fopen("/home/alexis/Documents/Tec/SO/Scheduling-Cars/scheduler.txt", "r");
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

GCallback paint_car(GtkWidget* widget, cairo_t *cr, gpointer data) {
    if (start_drawing_cars == 1) {
        const CarDraw * car_draw = (CarDraw*)data;
        const int xPos = car_draw->x;
        int laneAdjust = 0;
        if (car_draw->dir == LEFT) {
            laneAdjust = 53;
        }

        if (car_draw->type == NORMAL) {
            set_cairo_color(cr, 102, 178, 255, 255);//Cuerpo del cuarto
            cairo_rectangle(cr, ROAD_X+10+xPos, ROAD_Y+10+laneAdjust, 50, ROAD_HEIGHT/2-20);
            cairo_fill(cr);
            int directionOffset = xPos;
            if (car_draw->dir == LEFT) {
                directionOffset += 45;
            }
            set_cairo_color(cr, 255, 255, 0, 255);//Luces
            cairo_rectangle(cr, ROAD_X+10+directionOffset, ROAD_Y+14+laneAdjust, 5, 5);
            cairo_fill(cr);

            cairo_rectangle(cr, ROAD_X+10+directionOffset, ROAD_Y+31+laneAdjust, 5, 5);
            cairo_fill(cr);
        }
        else if (car_draw->type == SPORT) {
            set_cairo_color(cr, 255, 128, 0, 255);//Cuerpo del carro
            cairo_rectangle(cr, ROAD_X+10+xPos, ROAD_Y+10+laneAdjust, 50, ROAD_HEIGHT/2-20);
            cairo_fill(cr);

            set_cairo_color(cr, 0, 0, 0, 255);//Detalles
            cairo_rectangle(cr, ROAD_X+10+xPos, ROAD_Y+14+laneAdjust, 50, 5);
            cairo_fill(cr);

            set_cairo_color(cr, 0, 0, 0, 255);
            cairo_rectangle(cr, ROAD_X+10+xPos, ROAD_Y+31+laneAdjust, 50, 5);
            cairo_fill(cr);
        }else {
            set_cairo_color(cr, 255, 255, 255, 255);
            cairo_rectangle(cr, ROAD_X+10+xPos, ROAD_Y+10+laneAdjust, 50, ROAD_HEIGHT/2-20);
            cairo_fill(cr);
            int directionOffset = xPos;
            if (car_draw->dir == LEFT) {
                directionOffset += 32;
            }
            set_cairo_color(cr, 255, 0, 0, 255);
            cairo_rectangle(cr, ROAD_X+15+directionOffset, ROAD_Y+20+laneAdjust, 5, 9);
            cairo_fill(cr);
        }
    }
    return FALSE;
}

double getElapsedTime(clock_t start, clock_t end) {
    return ((double)(end - start)) / CLOCKS_PER_SEC;
}

void playCarMotion(double travel_time_seconds, double total_travel_time) {
    clock_t begin_time = clock();
    double time_elapsed = getElapsedTime(begin_time, clock());
    double advanceTime = total_travel_time/(double)(ROAD_WIDTH-70);
    clock_t lastRefresh = clock();

    while (time_elapsed < travel_time_seconds) {
        if (getElapsedTime(lastRefresh, clock()) >= advanceTime) {
            if (car_drawn->dir == LEFT) {
                car_drawn->x +=1;
            }else {
                car_drawn->x -=1;
            }
            lastRefresh = clock();
        }
        time_elapsed = getElapsedTime(begin_time, clock());
    }
}

void* car_thread(void* arg) {
    Car* car = (Car*)arg;
    //printf("Here car %d\n", car->id);
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

            // Wait with timeout
            struct timespec wait_time = {
                .tv_sec = 0,
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
                .tv_sec = 0,
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
                    .tv_sec = 0,
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
                    .tv_sec = 0,
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
                    .tv_sec = 0,
                    .tv_nsec = 100000000 // 100ms
                };
                CEcond_timedwait(&road_cond, &road_mutex, &wait_time);
            }
        }
    }


    cars_on_road++;

    if (car->dir == LEFT) {
        cars_on_road_left++;
    } else {
        cars_on_road_right++;
    }
    road_occupied_dir = car->dir;

    // Add car to visualization when it enters the road
    //add_car_visual(car->id, car->dir, car->type);

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

    // For visualization - update position incrementally
    struct timespec start_time;
    clock_gettime(CLOCK_REALTIME, &start_time);
    double travel_time_seconds = travel_time_us / 1000000L;
    car_drawn->type = car->type;
    car_drawn->dir = car->dir;
    car_drawn->x = car->x;
    // For Round Robin, check if time slice expires during travel
    if (current_scheduler == RR && car->type != EMERGENCY) {
        // Calculate how long the car will take to cross
        // Check if car will complete crossing within time slice
        if (travel_time_seconds <= time_slice_remaining) {
            // Car completes crossing within time slice
            playCarMotion(travel_time_seconds, road_length / speed);
            car->x = car_drawn->x;
        } else {
            // Car's time slice expires during crossing
            // Let it continue anyway since it's already on the road
            // But record that it exceeded its time slice
            printf("[RR] Car %d exceeded time slice but continuing to cross.\n", car->id);
            playCarMotion(travel_time_seconds, road_length / speed);
            car->x = car_drawn->x;
            rr_timeout = 1;
        }
    } else {
        // Regular crossing for other schedulers
        playCarMotion(travel_time_seconds, road_length / speed);
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

    printf("Remaining L: %d Reamining R: %d \n", remaining_left, remaining_right);

    if (remaining_left == 0 && remaining_right == 0) {
        printf("Finished \n");
        start_drawing_cars = 0;
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


// Update the GUI (called from main thread)
gboolean update_gui(gpointer data) {
    GtkWidget* drawing_area = GTK_WIDGET(data);

    // Request redraw of the drawing area
    gtk_widget_queue_draw(drawing_area);

    // Continue timer if simulation is running
    return simulation_running;
}

void spawn_new(SpawnCarsParams * params) {
    Car* c = malloc(sizeof(Car));
    c->id = ++(*params->id);
    c->dir = params->dir;
    c->type = params->type;
    if (c->dir == LEFT) {
        remaining_left++;
        c->x = 10;
    }else {
        remaining_right++;
        c->x = ROAD_WIDTH-60;
    }
    CEthread_t tid;
    CEthread_create(&tid, NULL, car_thread, c);
}

GCallback spawn_cars(GtkWidget *widget, GdkEventButton event, gpointer * data) {
    SpawnCarsParams *params = (SpawnCarsParams*) data;
    spawn_new(params);
}



// Drawing function for the GTK drawing area
gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    // Set background color
    set_cairo_color(cr, 220, 220, 220, 255);  // Light gray
    cairo_paint(cr);

    // Draw road
    set_cairo_color(cr, 80, 80, 80, 255);  // Dark gray
    cairo_rectangle(cr, ROAD_X, ROAD_Y, ROAD_WIDTH, ROAD_HEIGHT);
    cairo_fill(cr);

    // Draw road markers
    set_cairo_color(cr, 255, 255, 255, 255);  // White
    for (int x = ROAD_X + 20; x < ROAD_X + ROAD_WIDTH; x += 40) {
        cairo_rectangle(cr, x, ROAD_Y+ ROAD_HEIGHT/2, 20, 4);
        cairo_fill(cr);
    }


    // Draw labels for queues
    set_cairo_color(cr, 0, 0, 0, 255);  // Black text
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);

    cairo_move_to(cr, 50, 80);
    cairo_show_text(cr, "Left:");

    cairo_move_to(cr, 50, 95);
    char textLeft [2];
    sprintf(textLeft, "%d", remaining_left);
    cairo_show_text(cr, textLeft);

    cairo_move_to(cr, WINDOW_WIDTH/2 - 25, 80);
    cairo_show_text(cr, "Dirección:");

    cairo_move_to(cr, WINDOW_WIDTH/2 +10, 95);

    if (current_dir == LEFT) {
        char textDir [2] = "->";
        cairo_show_text(cr, textDir);
    }else {
        char textDir [2] = "<-";
        cairo_show_text(cr, textDir);
    }



    cairo_move_to(cr, WINDOW_WIDTH - 150, 80);
    cairo_show_text(cr, "Right:");
    cairo_move_to(cr, WINDOW_WIDTH - 150, 95);
    char textRight [2];
    sprintf(textRight, "%d", remaining_right);
    cairo_show_text(cr, textRight);

    return FALSE;
}

GCallback startRunningSimulation() {

    if (remaining_left + remaining_right > 0) {
        // Create a thread for simulation
        start_drawing_cars = 1;
        pthread_t simulation_thread;
        pthread_create(&simulation_thread, NULL, simulation_main, NULL);

        // Mark simulation as complete
        simulation_running = TRUE;

        // Wait for simulation thread to complete
        pthread_detach(simulation_thread);
        //CEthread_join(simulation_thread, NULL);
    }
}

GCallback change_car_type(GtkWidget *widget, GdkEventButton event, gpointer * data) {
    SpawnCarsParams *params = (SpawnCarsParams*) data;
    GtkComboBoxText * combo_box = (GtkComboBoxText *)widget;
    const gchar * setType = gtk_combo_box_text_get_active_text(combo_box);
    if (g_ascii_strcasecmp(setType, "Normal") == 0) {
        params->type = NORMAL;
    }else if (g_ascii_strcasecmp(setType, "Deportivo") == 0) {
        params->type = SPORT;
    }else {
        params->type = EMERGENCY;
    }
}

void init_gui(int* argc, char*** argv, int * id, SpawnCarsParams * paramsLeft, SpawnCarsParams * paramsRight) {
    gtk_init(argc, argv);

    // Create main window
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Traffic Simulation");
    gtk_window_set_default_size(GTK_WINDOW(window), WINDOW_WIDTH, WINDOW_HEIGHT);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // Create a vertical box
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Create drawing area
    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, WINDOW_WIDTH - 20, WINDOW_HEIGHT - 20);
    g_signal_connect(G_OBJECT(drawing_area), "draw", G_CALLBACK(on_draw), NULL);

    g_signal_connect(G_OBJECT(drawing_area), "draw", G_CALLBACK(paint_car), car_drawn);
    gtk_box_pack_start(GTK_BOX(vbox), drawing_area, TRUE, TRUE, 0);
    // Add control buttons
    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_end(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    //Botón de Spawn izquierdo
    GtkWidget* spawn_left_button = gtk_button_new_with_label("Spawn");
    paramsLeft->dir = LEFT;
    paramsLeft->id = id;
    paramsLeft->count = normales_left;
    paramsLeft->type = NORMAL;
    g_signal_connect(spawn_left_button, "clicked", G_CALLBACK(spawn_cars), paramsLeft);
    gtk_box_pack_start(GTK_BOX(hbox), spawn_left_button, FALSE, FALSE, 0);

    //Combo box para Spawn Izquierdo
    GtkWidget * comboLeft = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(comboLeft), "Normal");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(comboLeft), "Deportivo");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(comboLeft), "Emergencia");
    gtk_combo_box_set_active(GTK_COMBO_BOX(comboLeft), 0);
    gtk_box_pack_start(GTK_BOX(hbox), comboLeft, FALSE, FALSE, 0);
    g_signal_connect(comboLeft, "changed", G_CALLBACK(change_car_type), paramsLeft);

    //Botón de spawn derecho
    GtkWidget* spawn_right_button = gtk_button_new_with_label("Spawn");
    paramsRight->id = id;
    paramsRight->dir = RIGHT;
    paramsRight->count = normales_right;
    paramsRight->type = NORMAL;
    g_signal_connect(spawn_right_button, "clicked", G_CALLBACK(spawn_cars), paramsRight);
    gtk_box_pack_end(GTK_BOX(hbox), spawn_right_button, FALSE, FALSE, 0);

    //Combobox para Spawn Derecho
    GtkWidget * comboRight = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(comboRight), "Normal");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(comboRight), "Deportivo");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(comboRight), "Emergencia");
    gtk_combo_box_set_active(GTK_COMBO_BOX(comboRight), 0);
    gtk_box_pack_end(GTK_BOX(hbox), comboRight, FALSE, FALSE, 0);
    g_signal_connect(comboRight, "changed", G_CALLBACK(change_car_type), paramsRight);

    //Botón de correr
    GtkWidget* run_button = gtk_button_new_with_label("Run");
    g_signal_connect(run_button, "clicked", G_CALLBACK(startRunningSimulation), NULL);
    gtk_box_set_center_widget(GTK_BOX(hbox), run_button);

    // Show all widgets
    gtk_widget_show_all(window);



    // Start timer for regular updates (60 FPS)
    g_timeout_add(16, update_gui, drawing_area);
}



// Simulation thread function
void* simulation_main(void* arg) {
    // Initialize synchronization and state

    printf("Starting Simulation\n");
    // Wait until all cars have crosseds
    while ((remaining_left > 0 || remaining_right > 0) && simulation_running) {
        usleep(100000); // Sleep 100ms to avoid busy waiting
        CEthread_yield(); // Add yield to give other threads a chance to run

    }

    CEmutex_destroy(&road_mutex);
    CEcond_destroy(&road_cond);
    CEmutex_destroy(&queue_mutex);
    printf("Simulation stopped\n");

    return NULL;
}

void spawnInitialCars(Direction dir, int id,int norm, int sp, int em) {
    const CarType typeList [3] = {NORMAL, SPORT, EMERGENCY};
    const int carAmounts [3] = {norm, sp, em};
    SpawnCarsParams * newCar = malloc(sizeof(SpawnCarsParams));;
    newCar->dir = dir;

    for (int i = 0; i < 3; i++) {
        newCar->type = typeList[i];
        newCar->id = &id;
        for (int j = 0; j < carAmounts[i]; j++) {
            spawn_new(newCar);
        }
    }
    free(newCar);
}


int main(int argc, char* argv[]) {
    printf("Road Crossing Simulation \n");

    // Initialize the CEThreads library
    CEthread_lib_init();

    // Initialize queues
    init_queue(&left_queue);
    init_queue(&right_queue);

    // Initialize visualization mutex
    CEmutex_init(&visual_mutex, NULL);

    // Initialize car visualization array
    for (int i = 0; i < MAX_CARS_VISUAL; i++) {
        cars_visual[i].active = 0;
    }

    // Read config
    FILE* fp = fopen("/home/alexis/Documents/Tec/SO/Scheduling-Cars/config.txt", "r");
    if (!fp) {
        printf("Failed to open config file\n");
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
    // ... (rest of configuration and initialization unchanged)
    read_scheduler_config();
    CEmutex_init(&road_mutex, NULL);
    CEcond_init(&road_cond, NULL);
    CEmutex_init(&queue_mutex, NULL);

    int id = 0;
    spawnInitialCars(LEFT, id, normales_left,deportivos_left,emergencia_left);
    spawnInitialCars(RIGHT, id, normales_right, deportivos_right, emergencia_right);


    printf("Remaining L: %d Reamining R: %d \n", remaining_left, remaining_right);

    cars_in_window  = 0;
    current_dir     = LEFT;

    // Launch signal thread if needed
    CEthread_t tidSignal;
    if (!strcmp(flow_method, "SIGNAL")) {
        CEthread_create(&tidSignal, NULL, signal_thread, NULL);
    }
    // Initialize GUI

    simulation_running = TRUE;

    SpawnCarsParams * paramsLeft = malloc(sizeof(SpawnCarsParams));
    SpawnCarsParams * paramsRight = malloc(sizeof(SpawnCarsParams));
    car_drawn = malloc(sizeof(CarDraw));

    init_gui(&argc, &argv, &id, paramsLeft, paramsRight);


    // Start the GTK main loop

    gtk_main();

    free(car_drawn);
    free(paramsLeft);
    free(paramsRight);
    // Cleanup
    /*CEmutex_destroy(&road_mutex);
    CEcond_destroy(&road_cond);
    CEmutex_destroy(&queue_mutex);*/
    CEmutex_destroy(&visual_mutex);

    // Clean up the CEThreads library
    CEthread_lib_destroy();

    printf("Simulation complete. All vehicles have crossed.\n");
    return 0;
}


