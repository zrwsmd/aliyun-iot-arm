#ifndef TRACE_SIMULATOR_H
#define TRACE_SIMULATOR_H

#include <pthread.h>

struct AppContext;

typedef struct TraceSimulator {
    struct AppContext *app;
    pthread_t thread;
    int running;
    int period_ms;
    int batch_size;
    int send_interval_ms;
    long max_batches;
} TraceSimulator;

int trace_simulator_init(TraceSimulator *simulator, struct AppContext *app);
void trace_simulator_start(TraceSimulator *simulator);
void trace_simulator_stop(TraceSimulator *simulator);

#endif
