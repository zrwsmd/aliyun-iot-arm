#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>

#include "aiot_mqtt_api.h"
#include "device_config.h"
#include "deploy_manager.h"
#include "ide_connection_manager.h"
#include "start_manager.h"
#include "trace_simulator.h"

typedef struct AppContext {
    DeviceConfig config;
    char mqtt_host[256];
    void *mqtt_handle;
    pthread_mutex_t mqtt_lock;
    volatile sig_atomic_t running;
    pthread_t mqtt_process_thread;
    pthread_t mqtt_recv_thread;
    int mqtt_process_thread_running;
    int mqtt_recv_thread_running;
    int adas_switch;
    IdeConnectionManager ide_manager;
    DeployManager deploy_manager;
    StartManager start_manager;
    TraceSimulator trace_simulator;
} AppContext;

#endif
