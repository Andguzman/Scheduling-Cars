#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

typedef enum { LEFT = 0, RIGHT = 1 } Direction;

typedef struct {
    int id;
    Direction dir;
} Car;

pthread_mutex_t road_mutex;
pthread_cond_t  road_cond;

// Configuration parameters
char flow_method[16];      // "FIFO" or "EQUITY"
int road_length;            // units
int car_speed;              // units per second (used to compute crossing time)
int num_left, num_right;
int W;                      // equity window size

// State for EQUITY method
Direction current_dir;
int cars_in_window;
int remaining_left, remaining_right;

void* car_thread(void* arg) {
    Car* car = (Car*)arg;
    long travel_time_us = (road_length * 1000000L) / car_speed;

    printf("[Arrive] Car %d from %s side.\n",
           car->id,
           car->dir == LEFT ? "LEFT" : "RIGHT");

    pthread_mutex_lock(&road_mutex);

    if (strcmp(flow_method, "FIFO") == 0) {
        // FIFO: as soon as road is free, any waiting car can go
        // road_mutex serializes access
    }
    else if (strcmp(flow_method, "EQUITY") == 0) {
        // EQUITY: allow W cars from one side, then switch
        while (car->dir != current_dir || cars_in_window >= W) {
            // if no cars remain on current side, force switch
            if ((current_dir == LEFT  && remaining_left  == 0) ||
                (current_dir == RIGHT && remaining_right == 0)) {
                cars_in_window = 0;
                current_dir = car->dir;
                pthread_cond_broadcast(&road_cond);
            } else {
                pthread_cond_wait(&road_cond, &road_mutex);
            }
        }
    }

    // Enter the road
    printf("[Enter ] Car %d from %s side.\n",
           car->id,
           car->dir == LEFT ? "LEFT" : "RIGHT");

    // Simulate crossing (road is critical section)
    usleep(travel_time_us);

    // Exit the road
    printf("[Exit  ] Car %d from %s side.\n",
           car->id,
           car->dir == LEFT ? "LEFT" : "RIGHT");

    // Update equity state
    if (strcmp(flow_method, "EQUITY") == 0) {
        cars_in_window++;
        if (car->dir == LEFT)    remaining_left--;
        if (car->dir == RIGHT)   remaining_right--;

        if (cars_in_window >= W ||
            (current_dir == LEFT  && remaining_left  == 0) ||
            (current_dir == RIGHT && remaining_right == 0)) {
            cars_in_window = 0;
            current_dir = (current_dir == LEFT) ? RIGHT : LEFT;
        }
        pthread_cond_broadcast(&road_cond);
    }

    pthread_mutex_unlock(&road_mutex);
    free(car);
    return NULL;
}

int main() {
    printf("Simple Road Crossing Simulation\n");
    printf("================================\n");

    // Read configuration from console
    printf("Enter flow method (FIFO/EQUITY): ");
    if (scanf("%15s", flow_method) != 1) return 1;
    printf("Road length (units): ");
    if (scanf("%d", &road_length) != 1) return 1;
    printf("Car speed (units/sec): ");
    if (scanf("%d", &car_speed) != 1) return 1;
    printf("Number of cars on LEFT side: ");
    if (scanf("%d", &num_left) != 1) return 1;
    printf("Number of cars on RIGHT side: ");
    if (scanf("%d", &num_right) != 1) return 1;
    if (strcmp(flow_method, "EQUITY") == 0) {
        printf("Equity window W: ");
        if (scanf("%d", &W) != 1) return 1;
    }

    // Initialize state
    pthread_mutex_init(&road_mutex, NULL);
    pthread_cond_init(&road_cond, NULL);

    remaining_left  = num_left;
    remaining_right = num_right;
    cars_in_window  = 0;
    current_dir     = LEFT;

    // Spawn car threads
    pthread_t tid;
    int total = num_left + num_right;
    int created = 0;
    for (int i = 0; i < num_left; ++i) {
        Car* car = malloc(sizeof(Car));
        car->id = ++created;
        car->dir = LEFT;
        pthread_create(&tid, NULL, car_thread, car);
    }
    for (int i = 0; i < num_right; ++i) {
        Car* car = malloc(sizeof(Car));
        car->id = ++created;
        car->dir = RIGHT;
        pthread_create(&tid, NULL, car_thread, car);
    }

    // Wait for all cars to finish
    // (In a real project, you'd store all tids; here, for simplicity, sleep)
    sleep((road_length * (num_left + num_right)) / car_speed + 1);

    pthread_mutex_destroy(&road_mutex);
    pthread_cond_destroy(&road_cond);

    printf("Simulation complete.\n");
    return 0;
}
