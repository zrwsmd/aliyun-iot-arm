#ifndef DEPLOY_MANAGER_H
#define DEPLOY_MANAGER_H

#include <pthread.h>
#include <stddef.h>

struct AppContext;

typedef struct DeployManager {
    struct AppContext *app;
    pthread_mutex_t lock;
    int busy;
} DeployManager;

int deploy_manager_init(DeployManager *manager, struct AppContext *app);
void deploy_manager_cleanup(DeployManager *manager);
int deploy_manager_handle_deploy(DeployManager *manager, const char *params_json, char *response_json, size_t response_size);

#endif
