// Executes the real MQTT component against a pthread-backed RTOS/client fake.
// stop joins the callback thread, matching the deadlock-sensitive IDF contract.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fake_platform.h"
#include "credential_store.h"
#include "mqtt_connection.h"

struct mutex { pthread_mutex_t lock; };
struct events { pthread_mutex_t lock; pthread_cond_t changed; unsigned bits; };
struct client {
    pthread_t thread;
    atomic_bool stopped;
    bool fail;
    void (*callback)(void *, esp_event_base_t, int32_t, void *);
};
static atomic_int published;
static bool saved;
static char stored_host[128], stored_username[64], stored_password[96];
static uint16_t stored_port;

static void pause_briefly(void) { struct timespec t = {0, 100000}; nanosleep(&t, NULL); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct mutex *m = calloc(1, sizeof(*m)); assert(m); pthread_mutex_init(&m->lock, NULL); return m;
}
int xSemaphoreTake(SemaphoreHandle_t m, unsigned timeout) { (void)timeout; return pthread_mutex_lock(&m->lock) == 0; }
int xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(&m->lock) == 0; }
EventGroupHandle_t xEventGroupCreate(void) {
    struct events *e = calloc(1, sizeof(*e)); assert(e);
    pthread_mutex_init(&e->lock, NULL); pthread_cond_init(&e->changed, NULL); return e;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t e) {
    pthread_mutex_lock(&e->lock); unsigned bits = e->bits; pthread_mutex_unlock(&e->lock); return bits;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t bits) {
    pthread_mutex_lock(&e->lock); e->bits |= bits; pthread_cond_broadcast(&e->changed);
    unsigned result = e->bits; pthread_mutex_unlock(&e->lock); return result;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t bits) {
    pthread_mutex_lock(&e->lock); unsigned result = e->bits; e->bits &= ~bits;
    pthread_mutex_unlock(&e->lock); return result;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, int clear, int all, unsigned timeout) {
    (void)clear; (void)all; (void)timeout;
    pthread_mutex_lock(&e->lock);
    while (!(e->bits & bits)) pthread_cond_wait(&e->changed, &e->lock);
    unsigned result = e->bits; pthread_mutex_unlock(&e->lock); return result;
}

static atomic_int subscribed, delivered;
static void received(const char *payload, size_t length, bool retained, void *context) {
    (void)context;
    assert(length==4 && !strcmp(payload,"ping") && retained);
    atomic_fetch_add(&delivered,1);
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t client,const char *topic,int qos) {
    (void)client; assert(!strcmp(topic,"test/command") && qos==1);
    return atomic_fetch_add(&subscribed,1);
}
esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client) { (void)client; return ESP_OK; }
static void messages(struct client *client) {
    esp_mqtt_event_t event={.client=client,.topic="test/command",.topic_len=12,
        .data="pi",.data_len=2,.total_data_len=4,.retain=true};
    client->callback(NULL,NULL,MQTT_EVENT_DATA,&event);
    event.topic=NULL; event.topic_len=0; event.current_data_offset=2; event.data="ng";
    client->callback(NULL,NULL,MQTT_EVENT_DATA,&event);
    // Oversized and out-of-order fragments must not invoke application code.
    event.current_data_offset=0; event.topic="test/command"; event.topic_len=12;
    event.total_data_len=2000;
    client->callback(NULL,NULL,MQTT_EVENT_DATA,&event);
    event.total_data_len=4; event.current_data_offset=2;
    client->callback(NULL,NULL,MQTT_EVENT_DATA,&event);
}
static void *client_thread(void *arg) {
    struct client *client = arg;
    esp_mqtt_event_t event = {.client=client};
    pause_briefly();
    client->callback(NULL, NULL, client->fail ? MQTT_EVENT_ERROR : MQTT_EVENT_CONNECTED, &event);
    if (!client->fail) messages(client);
    return NULL;
}
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config) {
    assert(config->broker.address.transport == MQTT_TRANSPORT_OVER_SSL);
    assert(strcmp(config->broker.verification.certificate, "public-test-ca") == 0);
    assert(strcmp(config->session.last_will.msg, "offline") == 0);
    struct client *client = calloc(1, sizeof(*client)); assert(client);
    client->fail = strcmp(config->credentials.authentication.password, "reject") == 0;
    return client;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t client, int id,
    void (*callback)(void *, esp_event_base_t, int32_t, void *), void *arg) {
    (void)id; (void)arg; client->callback = callback; return ESP_OK;
}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client) {
    assert(pthread_create(&client->thread, NULL, client_thread, client) == 0); return ESP_OK;
}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client) {
    pthread_join(client->thread, NULL); atomic_store(&client->stopped, true); return ESP_OK;
}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client) {
    assert(atomic_load(&client->stopped)); free(client); return ESP_OK;
}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t client, const char *topic, const char *payload,
    int length, int qos, bool retain, bool store) {
    (void)length; (void)retain; assert(store); assert(qos == 1); assert(topic); assert(payload);
    assert(!atomic_load(&client->stopped)); pause_briefly();
    assert(!atomic_load(&client->stopped)); return atomic_fetch_add(&published, 1);
}

size_t strlcpy(char *out, const char *in, size_t size) {
    size_t length = strlen(in); if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(out, in, n); out[n] = 0; } return length;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle) {
    assert(strcmp(ns, "mqtt_cfg") == 0); *handle = 1; return mode == NVS_READONLY && !saved ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
static char *stored(const char *key) {
    if (!strcmp(key, "host")) return stored_host;
    if (!strcmp(key, "username")) return stored_username;
    assert(!strcmp(key, "password")); return stored_password;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *size) {
    (void)h; assert(strlen(stored(key)) < *size); strlcpy(out, stored(key), *size); return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) { (void)h; strcpy(stored(key), value); return ESP_OK; }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *value) { (void)h; assert(!strcmp(key, "port")); *value = stored_port; return ESP_OK; }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value) { (void)h; assert(!strcmp(key, "port")); stored_port = value; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; saved = true; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
void credential_store_clear_secret(void *buffer, size_t size) { memset(buffer, 0, size); }
esp_err_t credential_store_forget(const char *ns) { assert(!strcmp(ns, "mqtt_cfg")); saved = false; return ESP_OK; }

static void *publisher(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; ++i) {
        esp_err_t error = mqtt_connection_publish("test/state", "{}", true);
        assert(error == ESP_OK || error == ESP_ERR_INVALID_STATE);
        pause_briefly();
    }
    return NULL;
}

int main(void) {
    const mqtt_connection_options_t options = {.device_id="test", .ca_certificate="public-test-ca",
        .availability_topic="test/availability", .command_topic="test/command", .on_message=received};
    assert(mqtt_connection_initialize(&options) == ESP_OK);
    assert(mqtt_connection_start_saved() == ESP_ERR_NVS_NOT_FOUND);
    assert(mqtt_connection_test_and_save(NULL, 8883, "user", "pass") == ESP_ERR_INVALID_ARG);
    assert(mqtt_connection_test_and_save("broker", 8883, "user", "pass") == ESP_OK);
    assert(mqtt_connection_is_configured());
    assert(mqtt_connection_generation() == 1);
    assert(mqtt_connection_start_saved() == ESP_OK);
    assert(mqtt_connection_generation() == 1); // idempotence
    assert(mqtt_connection_test_and_save("bad-broker", 8883, "user", "reject") != ESP_OK);
    char host[128]; mqtt_connection_host(host, sizeof(host));
    assert(!strcmp(host, "broker")); // failed test restored previous settings
    pthread_t thread; assert(pthread_create(&thread, NULL, publisher, NULL) == 0);
    for (int i = 0; i < 100; ++i) {
        assert(mqtt_connection_forget() == ESP_OK);
        assert(!mqtt_connection_is_configured());
        assert(mqtt_connection_test_and_save("broker", 8883, "user", "pass") == ESP_OK);
    }
    pthread_join(thread, NULL);
    assert(mqtt_connection_forget() == ESP_OK);
    assert(!mqtt_connection_is_connected());
    assert(mqtt_connection_start_saved() == ESP_ERR_NVS_NOT_FOUND);
    assert(atomic_load(&published) > 100);
    assert(atomic_load(&subscribed)>1);
    assert(atomic_load(&delivered)==atomic_load(&subscribed));
    puts("MQTT lifecycle: 100 replacements with concurrent publishing, rollback, forget and TLS configuration passed");
    return 0;
}
