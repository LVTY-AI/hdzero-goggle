#include "log/record_control.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

#define PAYLOAD(type) (sizeof(type) - sizeof(long))

int record_control_open(bool server) {
    int queue = msgget(RECORD_CONTROL_KEY, 0);
    if (queue >= 0) {
        struct msqid_ds st;
        if (msgctl(queue, IPC_STAT, &st) || st.msg_perm.uid != geteuid() ||
            (st.msg_perm.mode & 0777) != 0600) { errno = EACCES; return -1; }
        if (!server) return queue;
        // Called only after the recorder's singleton check: discard stale protocol state.
        if (msgctl(queue, IPC_RMID, NULL)) return -1;
    }
    if (!server) return -1;
    return msgget(RECORD_CONTROL_KEY, IPC_CREAT | IPC_EXCL | 0600);
}

int record_control_send(int queue, const record_control_request_t *request) {
    return msgsnd(queue, request, PAYLOAD(*request), IPC_NOWAIT);
}

int record_control_receive(int queue, record_control_request_t *request) {
    memset(request, 0, sizeof(*request));
    ssize_t n = msgrcv(queue, request, PAYLOAD(*request), RECORD_CONTROL_TYPE, IPC_NOWAIT | MSG_NOERROR);
    if (n < 0)
        return 0;
    if (n != PAYLOAD(*request) || request->magic != RECORD_CONTROL_MAGIC ||
        !request->owner || !request->cookie || !request->sequence ||
        request->command < RECORD_HELLO || request->command > RECORD_PAUSE ||
        !memchr(request->config, '\0', sizeof(request->config)))
        return -1;
    return 1;
}

int record_control_reply(int queue, const record_control_request_t *request, int error, int recording) {
    record_control_reply_t reply = {
        .type = (long)request->owner + 1024, .magic = RECORD_CONTROL_MAGIC,
        .owner = request->owner, .cookie = request->cookie, .sequence = request->sequence,
        .command = request->command, .recorder = (uint32_t)getpid(),
        .error = error, .recording = recording,
    };
    return msgsnd(queue, &reply, PAYLOAD(reply), IPC_NOWAIT);
}

int record_control_ack(int queue, const record_control_request_t *request, record_control_reply_t *reply) {
    ssize_t n = msgrcv(queue, reply, PAYLOAD(*reply), (long)request->owner + 1024,
                       IPC_NOWAIT | MSG_NOERROR);
    if (n < 0)
        return errno == ENOMSG || errno == EINTR ? 0 : -1;
    // Ignore replies to old processes/requests. Never infer completion from record.dat.
    if (n != PAYLOAD(*reply) || reply->magic != RECORD_CONTROL_MAGIC ||
        reply->owner != request->owner || reply->cookie != request->cookie ||
        reply->sequence != request->sequence || reply->command != request->command ||
        !reply->recorder || (reply->recording != 0 && reply->recording != 1) || reply->error < 0)
        return 0;
    return 1;
}
