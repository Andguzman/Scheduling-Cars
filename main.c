// example.c
// A simple example demonstrating CEThreads usage

#include <stdio.h>
#include <stdlib.h>
#include "CEThreads.h"

#define NUM_THREADS 5

// A simple mutex for synchronization
CEmutex_t mutex;

// Function that each thread will execute
void* thread_function(void* arg) {
    int thread_id = *((int*)arg);

    printf("Thread %d: Starting\n", thread_id);

    // Use mutex for critical section
    CEmutex_lock(&mutex);
    printf("Thread %d: In critical section\n", thread_id);

    // Simulate some work
    for (int i = 0; i < 3; i++) {
        printf("Thread %d: Working... %d\n", thread_id, i);
        CEthread_yield(); // Yield to allow other threads to run
    }

    printf("Thread %d: Leaving critical section\n", thread_id);
    CEmutex_unlock(&mutex);

    printf( "Thread %d: Finished\n", thread_id);

    // Return the thread ID as exit value (just for demonstration)
    int* result = malloc(sizeof(int));
    *result = thread_id;
    return result;
}

int main() {
    // Initialize the thread library
    CEthread_lib_init();

    // Initialize mutex
    CEmutex_init(&mutex, NULL);

    // Create thread IDs and arguments
    CEthread_t threads[NUM_THREADS];
    int thread_args[NUM_THREADS];

    // Create threads
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_args[i] = i + 1;
        int ret = CEthread_create(&threads[i], NULL, thread_function, &thread_args[i]);
        if (ret != 0) {
            fprintf(stderr, "Error creating thread %d: %d\n", i, ret);
            exit(EXIT_FAILURE);
        }
        printf("Main: Created thread %d with ID %u\n", i+1, threads[i]);
    }

    // Join threads and collect results
    for (int i = 0; i < NUM_THREADS; i++) {
        void* retval;
        int ret = CEthread_join(threads[i], &retval);
        if (ret != 0) {
            fprintf(stderr, "Error joining thread %d: %d\n", i, ret);
            continue;
        }

        int* result = (int*)retval;
        printf("Main: Thread %d returned %d\n", i+1, *result);
        free(result);
    }

    // Destroy mutex
    CEmutex_destroy(&mutex);

    // Clean up the thread library
    CEthread_lib_destroy();

    printf("Main: All threads have completed\n");
    return 0;
}