// 쓰려나
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "global.h"
#include "msg_type.h"
#include "shm_type.h"

/* 큐 핸들 전역변수(원본과 동일한 이름 유지) */
int IG_SERVER_Q, LED_Q, M30_Q, CONNECTION_Q;

#ifdef _WIN32
/* ===================== Windows(UCRT64) 경로 ===================== */
/* System-V 메시지큐가 없으므로, 빌드/연결만 되도록 더미 구현 제공 */

static int _next_id = 100;
static int _alloc_id(void) { return _next_id++; }

void msg_dump(int msg_qid)                { (void)msg_qid; }
int  msg_create(int msq_key)              { (void)msq_key; return _alloc_id(); }
void msg_clear(int msg_qid)               { (void)msg_qid; }
BOOL msg_delete(int msg_qid)              { (void)msg_qid; return TRUE; }
BOOL msg_all_delete(void)                 { return TRUE; }
BOOL msg_all_open(void)                   { return msg_all_create(); }
BOOL msg_all_create(void)
{
    IO_Control_Q = _alloc_id();
    Host_Q       = _alloc_id();
    VMS_Q        = _alloc_id();
    Radar_Q      = _alloc_id();
    Video_Q      = _alloc_id();
    Scenario_Q   = _alloc_id();
    RTU_Q        = _alloc_id();
    Monitor_Q    = _alloc_id();
    return TRUE;
}

#else
/* ========================= Linux 경로 ========================= */
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

/* 모드 매크로가 없으면 기본값 지정 */
#ifndef SVMSG_MODE
#define SVMSG_MODE 0666
#endif

void msg_dump(int msg_qid)
{
    struct msqid_ds info;
    if (msgctl(msg_qid, IPC_STAT, &info) == -1) {
        perror("msgctl(IPC_STAT)");
        return;
    }
    printf("qid=%d qnum=%lu qbytes=%lu\n",
           msg_qid,
           (unsigned long)info.msg_qnum,
#ifdef __linux__
           (unsigned long)info.__msg_cbytes   /* 리눅스 확장필드 */
#else
           (unsigned long)info.msg_cbytes
#endif
    );
}

int msg_create(int msq_key)
{
    int qid = msgget((key_t)msq_key, SVMSG_MODE | IPC_CREAT);
    if (qid == -1) perror("msgget");
    return qid;
}

/* msg_type.h 안의 키 상수 이름에 맞춰 수정하세요. */
BOOL msg_all_create(void)
{
    IG_SERVER_Q = msg_create(SVMSG_KEY_IG_SERVER_MANAGER_Q);
    LED_Q       = msg_create(SVMSG_KEY_LED_MANAGER_Q);
    M30_Q       = msg_create(SVMSG_KEY_M30_MANAGER_Q);
    CONNECTION_Q      = msg_create(SVMSG_KEY_CONNECTION_MANAGER_Q);
    return TRUE;
}

BOOL msg_all_open(void) { return msg_all_create(); }

void msg_clear(int msg_qid)
{
    struct { long mtype; char mtext[2048]; } msg;
    while (msgrcv(msg_qid, &msg, sizeof(msg.mtext), 0, IPC_NOWAIT) >= 0) {}
}

BOOL msg_delete(int msg_qid)
{
    return (msgctl(msg_qid, IPC_RMID, NULL) == 0) ? TRUE : FALSE;
}
BOOL msg_all_delete(void)
{
    bool ok = true;
    ok &= msg_delete(IG_SERVER_Q);
    ok &= msg_delete(LED_Q);
    ok &= msg_delete(M30_Q);
    ok &= msg_delete(CONNECTION_Q);
    return ok ? TRUE : FALSE;
}

/* 전체 큐 덤프 */
void msg_all_dump(void)
{
    printf("[MSG] --- Linux SysV message queues ---\n");
    printf("  IG_SERVER_Q   = %d\n", IG_SERVER_Q);
    printf("  LED_Q         = %d\n", LED_Q);
    printf("  M30_Q         = %d\n", M30_Q);
    printf("  CONNECTION_Q  = %d\n", CONNECTION_Q);
}
#endif