#ifndef IOT_IDE_APP_H
#define IOT_IDE_APP_H

#include "app_context.h"

int app_init(AppContext *app, const char *config_path);
int app_start(AppContext *app);
void app_shutdown(AppContext *app);
long long app_now_ms(void);
int app_publish_topic(AppContext *app, const char *topic, const char *payload, uint8_t qos);
int app_subscribe_topic(AppContext *app, const char *topic);
int app_post_properties(AppContext *app, const char *params_json);
int app_reply_service(AppContext *app, const char *service_path, const char *request_id, int code, const char *data_json);
int app_publish_trace(AppContext *app, const char *payload_json);

#endif
