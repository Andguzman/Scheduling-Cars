#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

// Simulación de cruce de una sola vía con modos FIFO, EQUITY y LETRERO (SIGNAL)
// Parámetros leídos desde archivo "config.txt"
// Emergencia: vehículo real-time hard, no puede esperar más que max_wait_emergency (s)

typedef enum { LEFT = 0, RIGHT = 1 } Direction;
typedef enum { NORMAL = 0, SPORT = 1, EMERGENCY = 2 } CarType;

typedef struct {
    int id;
    Direction dir;
    CarType type;
    struct timespec arrival_time; // For tracking waiting time for emergency vehicles
} Car;

pthread_mutex_t road_mutex;
pthread_cond_t road_cond;
pthread_mutex_t queue_mutex; // For managing the ready queue safely

// Car queue implementation for ordered priority queue
typedef struct CarQueueNode {
    Car* car;
    int priority;
    struct CarQueueNode* next;
} CarQueueNode;

typedef struct {
    CarQueueNode* head;
    CarQueueNode* tail;
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

// Initialize car queue
void init_queue(CarQueue* queue) {
    queue->head = NULL;
    queue->tail = NULL;
}

// Add car to queue based on priority
void enqueue_car(CarQueue* queue, Car* car) {
    pthread_mutex_lock(&queue_mutex);

    // Create new node
    CarQueueNode* new_node = (CarQueueNode*)malloc(sizeof(CarQueueNode));
    new_node->car = car;
    new_node->next = NULL;

    // Set priority based on car type
    // Emergency vehicles get highest priority
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
        pthread_mutex_unlock(&queue_mutex);
        return;
    }

    // For FIFO, just add to the end
    if (strcmp(flow_method, "FIFO") == 0) {
        queue->tail->next = new_node;
        queue->tail = new_node;
        pthread_mutex_unlock(&queue_mutex);
        return;
    }

    // Otherwise, insert based on priority (higher priority first)
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

    pthread_mutex_unlock(&queue_mutex);
}

// Get next car from queue
Car* dequeue_car(CarQueue* queue) {
    pthread_mutex_lock(&queue_mutex);

    if (queue->head == NULL) {
        pthread_mutex_unlock(&queue_mutex);
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

    pthread_mutex_unlock(&queue_mutex);
    return car;
}

// Check if there are any emergency vehicles in the queue
// whose wait time is approaching the deadline
int check_emergency_deadlines(CarQueue* queue) {
    pthread_mutex_lock(&queue_mutex);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    // No emergency vehicles waiting
    if ((queue == &left_queue && emergency_vehicles_waiting_left == 0) ||
        (queue == &right_queue && emergency_vehicles_waiting_right == 0)) {
        pthread_mutex_unlock(&queue_mutex);
        return 0;
    }

    CarQueueNode* node = queue->head;

    while (node != NULL) {
        if (node->car->type == EMERGENCY) {
            long wait_time = (now.tv_sec - node->car->arrival_time.tv_sec);

            // If emergency vehicle is approaching deadline (80% of max wait)
            if (wait_time >= (max_wait_emergency * 0.8)) {
                pthread_mutex_unlock(&queue_mutex);
                return 1;
            }
        }
        node = node->next;
    }

    pthread_mutex_unlock(&queue_mutex);
    return 0;
}

// Signal thread to change traffic direction
void* signal_thread(void* arg) {
    while (1) {
        sleep(signal_time);
        pthread_mutex_lock(&road_mutex);
        // Only change direction if there are no emergency vehicles approaching deadline
        // in the current direction
        if ((current_dir == LEFT && !check_emergency_deadlines(&left_queue)) ||
            (current_dir == RIGHT && !check_emergency_deadlines(&right_queue))) {
            current_dir = (current_dir == LEFT) ? RIGHT : LEFT;
            cars_in_window = 0;
            printf("[Signal] Cambio de sentido: %s\n",
                current_dir == LEFT ? "LEFT" : "RIGHT");
            pthread_cond_broadcast(&road_cond);
        } else {
            printf("[Signal] Maintaining direction due to emergency vehicle priority: %s\n",
                current_dir == LEFT ? "LEFT" : "RIGHT");
        }
        pthread_mutex_unlock(&road_mutex);
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

// Check if a car can enter based on flow control rules
int can_enter_road(Car* car) {
    // If road is empty, car can enter
    if (cars_on_road == 0) {
        return 1;
    }

    // If road has cars going in the same direction, car can enter
    if ((car->dir == LEFT && cars_on_road_left > 0 && cars_on_road_right == 0) ||
        (car->dir == RIGHT && cars_on_road_right > 0 && cars_on_road_left == 0)) {
        return 1;
    }

    // Otherwise, car cannot enter (road occupied by cars going in opposite direction)
    return 0;
}

void* car_thread(void* arg) {
    Car* car = (Car*)arg;

    // Record arrival time
    clock_gettime(CLOCK_REALTIME, &car->arrival_time);

    long speed = get_speed(car->type);
    long travel_time_us = (road_length * 1000000L) / speed;

    printf("[Arrive] Car %d [%s] from %s side \n",
        car->id,
        type_name(car->type),
        car->dir == LEFT ? "LEFT" : "RIGHT",
        car->arrival_time.tv_sec,
        car->arrival_time.tv_nsec);


    // Add car to appropriate queue
    if (car->dir == LEFT) {
        enqueue_car(&left_queue, car);
    } else {
        enqueue_car(&right_queue, car);
    }

    pthread_mutex_lock(&road_mutex);

    // Emergency vehicle handling with strict deadline enforcement
    if (car->type == EMERGENCY) {
        struct timespec deadline = {
            .tv_sec = car->arrival_time.tv_sec + max_wait_emergency,
            .tv_nsec = car->arrival_time.tv_nsec
        };

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);

        while (1) {
            // Check if deadline is approaching
            int remaining = time_to_deadline(car->arrival_time);

            // If emergency vehicle can enter now, break the loop
            if (can_enter_road(car)) {
                if (car->dir == LEFT) {
                    // Remove car from queue when it enters
                    Car* next_car = dequeue_car(&left_queue);
                    // Sanity check that we dequeued the correct car
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

            // Force entry if deadline is imminent (less than 1 second remaining)
            if (remaining <= 1) {
                printf("[EMERGENCY OVERRIDE] Car %d forcing entry with %d seconds remaining to deadline\n",
                       car->id, remaining);

                // Even with forcing entry, we need to respect that cars already on the road
                // must continue, but we'll get priority for next entry

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

            pthread_cond_timedwait(&road_cond, &road_mutex, &wait_time);
        }
    } else {
        // Regular car waiting logic based on flow control method
        if (strcmp(flow_method, "FIFO") == 0) {
            // FIFO: Wait until car is at front of its queue and can enter road
            while (1) {
                pthread_mutex_lock(&queue_mutex);
                int is_front = (car->dir == LEFT) ?
                    (left_queue.head && left_queue.head->car->id == car->id) :
                    (right_queue.head && right_queue.head->car->id == car->id);
                pthread_mutex_unlock(&queue_mutex);

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
                    pthread_cond_wait(&road_cond, &road_mutex);
                    continue;
                }

                // Wait with timeout
                struct timespec wait_time = {
                    .tv_sec = 0,
                    .tv_nsec = 100000000 // 100ms
                };
                pthread_cond_timedwait(&road_cond, &road_mutex, &wait_time);
            }
        }
        else if (strcmp(flow_method, "EQUITY") == 0) {
            // EQUITY: Wait until car's direction is allowed and it's within the window
            while (1) {
                pthread_mutex_lock(&queue_mutex);
                int is_front = (car->dir == LEFT) ?
                    (left_queue.head && left_queue.head->car->id == car->id) :
                    (right_queue.head && right_queue.head->car->id == car->id);
                pthread_mutex_unlock(&queue_mutex);

                int can_go = (car->dir == current_dir && cars_in_window < W && is_front) ||
                            (current_dir == LEFT && remaining_left == 0 && car->dir == RIGHT && is_front) ||
                            (current_dir == RIGHT && remaining_right == 0 && car->dir == LEFT && is_front);

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
                    pthread_cond_wait(&road_cond, &road_mutex);
                    continue;
                }

                // Wait with timeout
                struct timespec wait_time = {
                    .tv_sec = 0,
                    .tv_nsec = 100000000 // 100ms
                };
                pthread_cond_timedwait(&road_cond, &road_mutex, &wait_time);
            }
        }
        else if (strcmp(flow_method, "SIGNAL") == 0) {
            // SIGNAL: Wait until car's direction is allowed
            while (1) {
                pthread_mutex_lock(&queue_mutex);
                int is_front = (car->dir == LEFT) ?
                    (left_queue.head && left_queue.head->car->id == car->id) :
                    (right_queue.head && right_queue.head->car->id == car->id);
                pthread_mutex_unlock(&queue_mutex);

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
                    pthread_cond_wait(&road_cond, &road_mutex);
                    continue;
                }

                // Wait with timeout
                struct timespec wait_time = {
                    .tv_sec = 0,
                    .tv_nsec = 100000000 // 100ms
                };
                pthread_cond_timedwait(&road_cond, &road_mutex, &wait_time);
            }
        }
    }

    // Update road occupation counters
    cars_on_road++;
    if (car->dir == LEFT) {
        cars_on_road_left++;
    } else {
        cars_on_road_right++;
    }
    road_occupied_dir = car->dir;

    // Actual crossing
    printf("[Enter ] Car %d [%s] from %s side.\n",
           car->id, type_name(car->type),
           car->dir == LEFT ? "LEFT" : "RIGHT");

    // Release mutex while crossing to allow other cars to queue up
    pthread_mutex_unlock(&road_mutex);

    // Simulate crossing the road
    usleep(travel_time_us);

    // Update state
    pthread_mutex_lock(&road_mutex);

    // Update road occupation
    cars_on_road--;
    if (car->dir == LEFT) {
        cars_on_road_left--;
        remaining_left--;
    } else {
        cars_on_road_right--;
        remaining_right--;
    }

    printf("[Exit  ] Car %d [%s] from %s side.\n",
           car->id, type_name(car->type),
           car->dir == LEFT ? "LEFT" : "RIGHT");

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

    // Notify waiting cars
    pthread_cond_broadcast(&road_cond);
    pthread_mutex_unlock(&road_mutex);

    // Free car structure
    free(car);
    return NULL;
}

void spawn_cars(Direction side, CarType type, int count, int* id) {
    for (int i = 0; i < count; ++i) {
        Car* c = malloc(sizeof(Car));
        c->id = ++(*id);
        c->dir = side;
        c->type = type;

        pthread_t tid;
        pthread_create(&tid, NULL, car_thread, c);
        pthread_detach(tid);  // Detach thread to auto-cleanup when done
    }
}

int main() {
    printf("Road Crossing Simulation \n");

    // Initialize queues
    init_queue(&left_queue);
    init_queue(&right_queue);

    // Leer config
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

    // Inicializar sincronización y estado
    pthread_mutex_init(&road_mutex, NULL);
    pthread_cond_init(&road_cond, NULL);
    pthread_mutex_init(&queue_mutex, NULL);

    remaining_left  = normales_left + deportivos_left + emergencia_left;
    remaining_right = normales_right + deportivos_right + emergencia_right;
    cars_in_window  = 0;
    current_dir     = LEFT;

    // Lanzar hilo de señal si corresponde
    pthread_t tidSignal;
    if (!strcmp(flow_method, "SIGNAL")) {
        pthread_create(&tidSignal, NULL, signal_thread, NULL);
        pthread_detach(tidSignal);
    }

    // Crear carros
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
    }

    // Cleanup
    pthread_mutex_destroy(&road_mutex);
    pthread_cond_destroy(&road_cond);
    pthread_mutex_destroy(&queue_mutex);

    printf("Simulation complete. All vehicles have crossed.\n");
    return 0;
}