#include "trace_simulator.h"

#include <string.h>

int trace_simulator_init(TraceSimulator *simulator, struct AppContext *app) {
    if (simulator == NULL) {
        return -1;
    }

    memset(simulator, 0, sizeof(*simulator));
    simulator->app = app;
    return 0;
}

void trace_simulator_start(TraceSimulator *simulator) {
    (void)simulator;
}

void trace_simulator_stop(TraceSimulator *simulator) {
    (void)simulator;
}
