#include <log/record_control.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

int main(void) {
    int queue = record_control_open(true);
    assert(queue >= 0 && record_control_open(false) == queue);
    record_control_request_t request = {.type = RECORD_CONTROL_TYPE, .magic = RECORD_CONTROL_MAGIC,
        .owner = getpid(), .cookie = 77, .sequence = 1, .command = RECORD_STOP};
    record_control_request_t received;
    record_control_reply_t reply;
    assert(record_control_send(queue, &request) == 0);
    assert(record_control_receive(queue, &received) == 1);
    assert(received.sequence == request.sequence);
    assert(record_control_ack(queue, &request, &reply) == 0); // Not completed merely because sent/received.
    record_control_request_t stale = request;
    stale.cookie++;
    assert(record_control_reply(queue, &stale, 0, 0) == 0);
    assert(record_control_ack(queue, &request, &reply) == 0);
    stale = request; stale.sequence++;
    assert(record_control_reply(queue, &stale, 0, 0) == 0);
    assert(record_control_ack(queue, &request, &reply) == 0);
    assert(record_control_reply(queue, &request, EIO, 0) == 0);
    assert(record_control_ack(queue, &request, &reply) == 1);
    assert(reply.error == EIO && reply.recording == 0 && reply.recorder == (unsigned)getpid());
    request.magic--;
    assert(record_control_send(queue, &request) == 0);
    assert(record_control_receive(queue, &received) == -1);
    request.magic = RECORD_CONTROL_MAGIC;
    memset(request.config, 'x', sizeof(request.config));
    assert(record_control_send(queue, &request) == 0);
    assert(record_control_receive(queue, &received) == -1);
    request.config[0] = '\0';
    struct msqid_ds limits;
    assert(msgctl(queue, IPC_STAT, &limits) == 0);
    limits.msg_qbytes = sizeof(request) - sizeof(long);
    assert(msgctl(queue, IPC_SET, &limits) == 0);
    assert(record_control_send(queue, &request) == 0);
    assert(record_control_send(queue, &request) == -1 && errno == EAGAIN);
    assert(msgctl(queue, IPC_RMID, NULL) == 0);
    puts("DVR control: real IPC, delayed/stale replies, explicit errors, invalid payload and full queue passed");
    return 0;
}
