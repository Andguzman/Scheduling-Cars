#include <stdio.h>

int main(void) {
    printf("Hello, World!\n");
    return 0;
}


void CEthread_create() {
    printf("\n New thread");
    return;
}

void CEthread_join() {
    printf("\n Join");
}

void CEmutex_init() {
    printf("\n Init");

}

void CEmutex_destroy() {

    printf("\n Destroy");
}

void CEmutex_unlock() {
    printf("\n Unlock");
}