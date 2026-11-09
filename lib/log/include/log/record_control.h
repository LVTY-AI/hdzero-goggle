#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#define RECORD_CONTROL_KEY 0x48445203
#define RECORD_CONTROL_TYPE 3
#define RECORD_CONTROL_MAGIC 0x48445201u
#define RECORD_CONTROL_PATH 160
enum record_control_command { RECORD_HELLO = 1, RECORD_START, RECORD_STOP, RECORD_LIVE_STOP,
                              RECORD_CONFIG, RECORD_PAUSE };
typedef struct {
    long type;
    uint32_t magic, owner, cookie, sequence, command;
    char config[RECORD_CONTROL_PATH];
} record_control_request_t;
typedef struct {
    long type;
    uint32_t magic, owner, cookie, sequence, command, recorder;
    int32_t error, recording;
} record_control_reply_t;

int record_control_send(int queue, const record_control_request_t *request);
int record_control_open(bool server);
int record_control_receive(int queue, record_control_request_t *request);
int record_control_reply(int queue, const record_control_request_t *request, int error, int recording);
int record_control_ack(int queue, const record_control_request_t *request, record_control_reply_t *reply);
