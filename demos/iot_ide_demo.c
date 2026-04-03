#include "iot_ide_app.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static AppContext *g_app = NULL;

static void demo_handle_signal(int signo) {
    (void)signo;
    if (g_app != NULL) {
        g_app->running = 0;
    }
}

int main(int argc, char *argv[]) {
    const char *config_path = (argc > 1) ? argv[1] : "./device_id.json";
    AppContext app;

    memset(&app, 0, sizeof(app));
    g_app = &app;

    signal(SIGINT, demo_handle_signal);
    signal(SIGTERM, demo_handle_signal);

    if (app_init(&app, config_path) != 0) {
        fprintf(stderr, "app_init failed\n");
        return -1;
    }

    printf("=== iot-ide ===\n");
    printf("config: %s\n", config_path);
    printf("productKey: %s\n", app.config.product_key);
    printf("deviceName: %s\n", app.config.device_name);
    printf("mqttHost: %s\n", app.mqtt_host);

    if (app_start(&app) != 0) {
        fprintf(stderr, "app_start failed\n");
        app_shutdown(&app);
        return -1;
    }

    while (app.running) {
        sleep(1);
    }

    app_shutdown(&app);
    printf("iot-ide exit\n");
    return 0;
}
