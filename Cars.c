#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <cairo/cairo.h>
#include <gtk/gtk.h>

// Window dimensions
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 600

// Road dimensions
#define ROAD_WIDTH 100
#define ROAD_X (WINDOW_WIDTH / 2 - ROAD_WIDTH / 2)
#define ROAD_Y 50
#define ROAD_HEIGHT (WINDOW_HEIGHT - 100)

// Car dimensions
#define CAR_WIDTH 60
#define CAR_HEIGHT 40


// Replace printf statements with visualization calls
// In car_thread:
// Replace:
// printf("[Enter ] Car %d [%s] from %s side.\n", car->id, type_name(car->type), car->dir == LEFT ? "LEFT" : "RIGHT");
// With:
// add_visual_car(car->id, car->dir, car->type);

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

void spawn_cars(Direction side, CarType type, int count, int *id) {
    for (int i = 0; i < count; ++i) {
        Car *c = malloc(sizeof(Car));
        c->id = ++(*id);
        c->dir = side;
        c->type = type;

        pthread_t tid;
        pthread_create(&tid, NULL, car_thread, c);
        pthread_detach(tid); // Detach thread to auto-cleanup when done
    }
}

// Visualization data structure
typedef struct VisualCar {
    int id;
    Direction dir;
    CarType type;
    double x;
    double y;
    struct VisualCar* next;
} VisualCar;

// Global visualization variables
VisualCar* cars_on_screen = NULL;
GtkWidget* drawing_area;
GtkWidget* status_label;
gboolean simulation_running = TRUE;
pthread_mutex_t visual_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to create rgba color
static void set_cairo_color(cairo_t* cr, double r, double g, double b, double a) {
    cairo_set_source_rgba(cr, r/255.0, g/255.0, b/255.0, a/255.0);
}

// Add a car to the visual list
void add_visual_car(int id, Direction dir, CarType type) {
    pthread_mutex_lock(&visual_mutex);

    VisualCar* car = (VisualCar*)malloc(sizeof(VisualCar));
    car->id = id;
    car->dir = dir;
    car->type = type;

    // Position based on direction
    if (dir == LEFT) {
        car->x = 0;
        car->y = ROAD_Y + ROAD_HEIGHT / 4;
    } else {
        car->x = WINDOW_WIDTH - CAR_WIDTH;
        car->y = ROAD_Y + ROAD_HEIGHT * 3 / 4;
    }

    car->next = cars_on_screen;
    cars_on_screen = car;

    pthread_mutex_unlock(&visual_mutex);
}

// Remove a car from the visual list
void remove_visual_car(int id) {
    pthread_mutex_lock(&visual_mutex);

    VisualCar** pp = &cars_on_screen;
    while (*pp) {
        VisualCar* p = *pp;
        if (p->id == id) {
            *pp = p->next;
            free(p);
            pthread_mutex_unlock(&visual_mutex);
            return;
        }
        pp = &p->next;
    }

    pthread_mutex_unlock(&visual_mutex);
}

// Update car positions on the road
void update_visual_cars() {
    pthread_mutex_lock(&visual_mutex);

    VisualCar* car = cars_on_screen;
    VisualCar* prev = NULL;

    while (car != NULL) {
        // Update position based on direction and speed
        float speed = get_speed(car->type) / 10.0;  // Scale speed for visualization

        if (car->dir == LEFT) {
            car->x += speed;
            if (car->x > WINDOW_WIDTH) {
                // Car has reached the end of the road
                if (prev == NULL) {
                    cars_on_screen = car->next;
                } else {
                    prev->next = car->next;
                }
                VisualCar* temp = car;
                car = car->next;
                free(temp);
                continue;
            }
        } else {
            car->x -= speed;
            if (car->x < -CAR_WIDTH) {
                // Car has reached the end of the road
                if (prev == NULL) {
                    cars_on_screen = car->next;
                } else {
                    prev->next = car->next;
                }
                VisualCar* temp = car;
                car = car->next;
                free(temp);
                continue;
            }
        }

        prev = car;
        car = car->next;
    }

    pthread_mutex_unlock(&visual_mutex);
}

// Draw car on cairo context
void draw_car(cairo_t* cr, VisualCar* car) {
    // Set color based on car type
    if (car->type == NORMAL) {
        set_cairo_color(cr, 0, 0, 255, 255);  // Blue
    } else if (car->type == SPORT) {
        set_cairo_color(cr, 255, 165, 0, 255);  // Orange
    } else {  // EMERGENCY
        set_cairo_color(cr, 255, 0, 0, 255);  // Red
    }

    // Draw car body
    cairo_rectangle(cr, car->x, car->y, CAR_WIDTH, CAR_HEIGHT);
    cairo_fill_preserve(cr);

    // Draw car border
    set_cairo_color(cr, 0, 0, 0, 255);  // Black
    cairo_stroke(cr);

    // Draw car ID
    cairo_text_extents_t extents;
    char id_str[10];
    sprintf(id_str, "%d", car->id);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 14);
    cairo_text_extents(cr, id_str, &extents);

    double text_x = car->x + (CAR_WIDTH - extents.width) / 2;
    double text_y = car->y + (CAR_HEIGHT + extents.height) / 2;

    set_cairo_color(cr, 255, 255, 255, 255);  // White text
    cairo_move_to(cr, text_x, text_y);
    cairo_show_text(cr, id_str);
}

// Draw queue
void draw_queue(cairo_t* cr, CarQueue* queue, double queue_x, double queue_y) {
    pthread_mutex_lock(&queue_mutex);

    CarQueueNode* node = queue->head;
    int i = 0;

    while (node != NULL && i < 5) {  // Show at most 5 cars in queue
        // Set color based on car type
        if (node->car->type == NORMAL) {
            set_cairo_color(cr, 0, 0, 255, 255);  // Blue
        } else if (node->car->type == SPORT) {
            set_cairo_color(cr, 255, 165, 0, 255);  // Orange
        } else {  // EMERGENCY
            set_cairo_color(cr, 255, 0, 0, 255);  // Red
        }

        // Draw car body
        cairo_rectangle(cr, queue_x, queue_y + i * 50, CAR_WIDTH, CAR_HEIGHT);
        cairo_fill_preserve(cr);

        // Draw car border
        set_cairo_color(cr, 0, 0, 0, 255);  // Black
        cairo_stroke(cr);

        // Draw car ID
        cairo_text_extents_t extents;
        char id_str[10];
        sprintf(id_str, "%d", node->car->id);

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14);
        cairo_text_extents(cr, id_str, &extents);

        double text_x = queue_x + (CAR_WIDTH - extents.width) / 2;
        double text_y = queue_y + i * 50 + (CAR_HEIGHT + extents.height) / 2;

        set_cairo_color(cr, 255, 255, 255, 255);  // White text
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, id_str);

        node = node->next;
        i++;
    }

    pthread_mutex_unlock(&queue_mutex);
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
    for (int y = ROAD_Y + 20; y < ROAD_Y + ROAD_HEIGHT; y += 40) {
        cairo_rectangle(cr, ROAD_X + ROAD_WIDTH/2 - 2, y, 4, 20);
        cairo_fill(cr);
    }

    // Draw left queue
    draw_queue(cr, &left_queue, 50, 100);

    // Draw right queue
    draw_queue(cr, &right_queue, WINDOW_WIDTH - 50 - CAR_WIDTH, 100);

    // Draw cars on the road
    pthread_mutex_lock(&visual_mutex);
    VisualCar* car = cars_on_screen;
    while (car != NULL) {
        draw_car(cr, car);
        car = car->next;
    }
    pthread_mutex_unlock(&visual_mutex);

    // Draw labels for queues
    set_cairo_color(cr, 0, 0, 0, 255);  // Black text
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);

    cairo_move_to(cr, 50, 80);
    cairo_show_text(cr, "Left Queue");

    cairo_move_to(cr, WINDOW_WIDTH - 50 - 100, 80);
    cairo_show_text(cr, "Right Queue");

    return FALSE;
}

// Update the GUI (called from main thread)
gboolean update_gui(gpointer data) {
    // Update car positions
    update_visual_cars();

    // Request redraw
    gtk_widget_queue_draw(drawing_area);

    // Update status label
    char status[100];
    sprintf(status, "Method: %s | Direction: %s | Cars left: %d | Cars right: %d",
            flow_method,
            current_dir == LEFT ? "LEFT" : "RIGHT",
            remaining_left, remaining_right);
    gtk_label_set_text(GTK_LABEL(status_label), status);

    // Continue timer if simulation is running
    return simulation_running;
}

// Initialize GTK interface
void init_gui(int* argc, char*** argv) {
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

    // Create status label
    status_label = gtk_label_new("Traffic Simulation Starting...");
    gtk_box_pack_start(GTK_BOX(vbox), status_label, FALSE, FALSE, 0);

    // Create drawing area
    drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, WINDOW_WIDTH, WINDOW_HEIGHT - 40);
    g_signal_connect(G_OBJECT(drawing_area), "draw", G_CALLBACK(on_draw), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), drawing_area, TRUE, TRUE, 0);

    // Show all widgets
    gtk_widget_show_all(window);

    // Start timer for regular updates (60 FPS)
    g_timeout_add(16, update_gui, NULL);
}

// Main function (modified)
int main(int argc, char* argv[]) {
    // Initialize GTK
    init_gui(&argc, &argv);

    // Rest of your existing main function...

    // Start GTK main loop in a separate thread
    pthread_t gtk_thread;
    pthread_create(&gtk_thread, NULL, (void*(*)(void*))gtk_main, NULL);

    // ... existing simulation code ...

    // When simulation completes
    simulation_running = FALSE;

    // Wait for GTK thread to finish
    pthread_join(gtk_thread, NULL);

    return 0;
}
