#ifndef IDE_CONNECTION_MANAGER_H
#define IDE_CONNECTION_MANAGER_H

#include <pthread.h>
#include <stddef.h>

struct AppContext;

typedef struct IdeConnectionManager {
    struct AppContext *app;
    pthread_mutex_t lock;
    int connected;
    char current_client_id[128];
    char current_client_info[512];
    long long last_heartbeat_ms;
    pthread_t heartbeat_thread;
    int heartbeat_thread_running;
} IdeConnectionManager;

int ide_connection_manager_init(IdeConnectionManager *manager, struct AppContext *app);
void ide_connection_manager_cleanup(IdeConnectionManager *manager);
void ide_connection_manager_clear_cloud_state(IdeConnectionManager *manager);
int ide_connection_manager_handle_request_connect(IdeConnectionManager *manager, const char *params_json, char *response_json, size_t response_size);
int ide_connection_manager_handle_disconnect(IdeConnectionManager *manager, const char *params_json, char *response_json, size_t response_size);
int ide_connection_manager_handle_heartbeat(IdeConnectionManager *manager, const char *params_json, char *response_json, size_t response_size);
int ide_connection_manager_is_connected_client(IdeConnectionManager *manager, const char *client_id);
void ide_connection_manager_get_snapshot(IdeConnectionManager *manager, int *connected, char *client_id, size_t client_id_size, long long *heartbeat_ms);

#endif
