#ifndef START_MANAGER_H
#define START_MANAGER_H

#include <pthread.h>
#include <stddef.h>

struct AppContext;

typedef struct StartManager {
    struct AppContext *app;
    pthread_mutex_t lock;
    int busy;
} StartManager;

int start_manager_init(StartManager *manager, struct AppContext *app);
void start_manager_cleanup(StartManager *manager);
int start_manager_handle_start(StartManager *manager, const char *params_json, char *response_json, size_t response_size);

#endif
