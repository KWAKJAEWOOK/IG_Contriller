/*
    M30_Manager.c
    볼라드(M30) 장치 제어 및 상태 관리 프로세스
    1. 1:N TCP 소켓 통신 (다중 클라이언트)
    2. 설정 파일(공유메모리)에 등록된 모든 IP로 연결 시도
    3. 개별 연결에 대한 상태 관리 및 수신 처리
*/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"
#include "global/logger.h"

#define MAX_M30_DEVICES 64
#define RECV_BUF_SIZE 2048

// M30 장치 연결 관리 구조체
typedef struct {
    char ip[32];
    int port;
    int type; // 0:None, 1:IN, 2:LOAD, 3:OUT 등 구분용
    char name[32]; // 로그용 이름 (예: N_IN_1)
    
    THANDLEINDEX handle; // CommData 핸들
    bool connected;
    time_t last_connect_try;
    
    uint8_t recv_buf[RECV_BUF_SIZE]; // 개별 수신 버퍼
    size_t buf_len;
} M30_CONTEXT;

M30_CONTEXT g_m30_devs[MAX_M30_DEVICES];
int g_m30_count = 0;

time_t nowtime;

// Ctrl+C 핸들러
void handle_sigint(int sig) {
    printf("\nM30_Manager Terminating... (Signal: %d)\n", sig);
    for(int i=0; i<g_m30_count; i++) {
        if(g_m30_devs[i].handle != -1) CommClose(g_m30_devs[i].handle);
    }
    if (shm_close() != 0) printf("shm_close() failed\n");
    exit(0);
}

// ============================ 초기화 ============================
// 공유메모리의 IP 목록을 로컬 구조체로 로드
void init_device_list() {
    g_m30_count = 0;
    int port = system_set_ptr->m30_port;
    if(port <= 0) port = 7531; // Default

    // 그룹별 IP 리스트 등록
    #define ADD_GROUP(IP_LIST, NAME_PREFIX, TYPE_CODE) \
    for(int i=0; i<5; i++) { \
        if(strlen(IP_LIST[i]) > 6) { \
            M30_CONTEXT *ctx = &g_m30_devs[g_m30_count++]; \
            strncpy(ctx->ip, IP_LIST[i], 32); \
            ctx->port = port; \
            ctx->type = TYPE_CODE; \
            snprintf(ctx->name, 32, "%s_%d", NAME_PREFIX, i+1); \
            ctx->handle = -1; \
            ctx->connected = false; \
            ctx->buf_len = 0; \
            ctx->last_connect_try = 0; \
            if(g_m30_count >= MAX_M30_DEVICES) return; \
        } \
    }

    // 북쪽
    ADD_GROUP(system_set_ptr->m30_n_in_ip,   "N_IN",   1);
    ADD_GROUP(system_set_ptr->m30_n_load_ip, "N_LOAD", 2);
    ADD_GROUP(system_set_ptr->m30_n_out_ip,  "N_OUT",  3);
    
    // 동쪽
    ADD_GROUP(system_set_ptr->m30_e_in_ip,   "E_IN",   1);
    ADD_GROUP(system_set_ptr->m30_e_load_ip, "E_LOAD", 2);
    ADD_GROUP(system_set_ptr->m30_e_out_ip,  "E_OUT",  3);

    // 남쪽
    ADD_GROUP(system_set_ptr->m30_s_in_ip,   "S_IN",   1);
    ADD_GROUP(system_set_ptr->m30_s_load_ip, "S_LOAD", 2);
    ADD_GROUP(system_set_ptr->m30_s_out_ip,  "S_OUT",  3);

    // 서쪽
    ADD_GROUP(system_set_ptr->m30_w_in_ip,   "W_IN",   1);
    ADD_GROUP(system_set_ptr->m30_w_load_ip, "W_LOAD", 2);
    ADD_GROUP(system_set_ptr->m30_w_out_ip,  "W_OUT",  3);

    logger_log(LOG_LEVEL_INFO, "M30 Device List Initialized. Total: %d", g_m30_count);
}

// ============================ 연결 관리 ============================
void manage_connections() {
    for (int i = 0; i < g_m30_count; i++) {
        M30_CONTEXT *ctx = &g_m30_devs[i];

        if (ctx->connected) continue;

        // 3초 쿨타임
        if (nowtime - ctx->last_connect_try < 3) continue;

        // 연결 시도
        ctx->last_connect_try = nowtime;
        // logger_log(LOG_LEVEL_DEBUG, "Try Connect %s (%s)", ctx->name, ctx->ip);
        
        ctx->handle = CommInit(CONNECT, ctx->ip, ctx->port, false); // false: 개별 로그 너무 많음 방지
        
        if (ctx->handle != -1) {
            logger_log(LOG_LEVEL_INFO, "M30 Connected: %s (%s)", ctx->name, ctx->ip);
            ctx->connected = true;
            ctx->buf_len = 0;
        }
    }
}

// ============================ 패킷 처리 ============================
void process_m30_packet(M30_CONTEXT *ctx) {
    // M30 프로토콜 파싱 로직 (예시)
    // 예: 02 ... 03 패킷 구조 확인
    // if (ctx->buf_len > 0) ctx->buf_len = 0; // 처리 완료 가정
}

void packet_loop() {
    int nRead = 0;
    BOOL res;

    for (int i = 0; i < g_m30_count; i++) {
        M30_CONTEXT *ctx = &g_m30_devs[i];
        if (!ctx->connected || ctx->handle == -1) continue;

        size_t space = RECV_BUF_SIZE - ctx->buf_len;
        if (space == 0) {
            ctx->buf_len = 0; // 버퍼 리셋
            space = RECV_BUF_SIZE;
        }

        res = RecvBuf(ctx->handle, (char*)(ctx->recv_buf + ctx->buf_len), space, &nRead, 0);

        if (res == TRUE && nRead > 0) {
            ctx->buf_len += nRead;
            process_m30_packet(ctx);
        } else if (res == FALSE) {
            int err = GetLastCommError();
            if (err == DISCONNECTED || (nRead == 0 && err != TIME_OUT)) {
                logger_log(LOG_LEVEL_WARN, "M30 Disconnected: %s", ctx->name);
                CommClose(ctx->handle);
                ctx->handle = -1;
                ctx->connected = false;
            }
        }
    }
}

// ============================ 메시지 큐 처리 ============================
void message_analy(BYTE *_pData) {  // 외부 프로세스에서 받은 데이터 파싱
	if(_pData[0] != (0x80 | M30_MANAGER_Q_PID))
	{
		printf("massege id %02x %02x err~\n", _pData[0], (0x80 | M30_MANAGER_Q_PID));
		return;
	}
	switch(_pData[1]) //opcode
	{
		case 0x40: {	// todo. 추가 예정?
            break;
        }
    }
}

void *do_thread(void *data) {
	int st_200ms_cnt = 0;
	int st_500ms_cnt = 0;
	int st_1s_cnt = 0;
	int st_5s_cnt = 0;
	BYTE recv_msg[1024];
	int recv_size = 0;
	bool tmp_msg_check = false;

	while (1)
	{
		usleep(100000);  //100ms
		if(st_200ms_cnt++ >= 1)
		{
			st_200ms_cnt = 0;
            nowtime = time(NULL);
		}
		if(st_500ms_cnt++ >= 4)
		{
			st_500ms_cnt = 0;
		}
		if(st_1s_cnt++ >= 9)
		{
			st_1s_cnt = 0;
			// if ((nowtime-last_keep_alive_time) >= 1) {	// 1초 이상 데이터가 안 들어오면
			// 	logger_log(LOG_LEVEL_ERROR, "1초 이상 수신 데이터 없음. 소켓 해제 및 재연결 시도");
			// 	close(HandleIndex);
			// 	HandleIndex = -1;
			// 	bConnected = FALSE;
			// }
		}
		if(st_5s_cnt++ >= 49)
		{
			st_5s_cnt = 0;
		}

        // 링버퍼 읽기
        recv_size = 0;
        tmp_msg_check = false;
        while (system_set_ptr->msg_M30_Q.head != system_set_ptr->msg_M30_Q.tail) {
            recv_msg[recv_size++] = system_set_ptr->msg_M30_Q.buffer[system_set_ptr->msg_M30_Q.tail];
            system_set_ptr->msg_M30_Q.tail++;
            if (system_set_ptr->msg_M30_Q.tail >= BUF_SIZE)
                system_set_ptr->msg_M30_Q.tail = 0;
            tmp_msg_check = true;
        }

        if (tmp_msg_check) {
            message_analy(recv_msg);
        }
    }
}

// ============================ MAIN ============================
int main() {
    if (shm_all_open() == false) {
        printf("M30_Mgr] shm_init failed\n");
        exit(-1);
    }
    if (msg_all_open() == false) {
        printf("M30_Mgr] msg_init failed\n");
    }

    signal(SIGINT, handle_sigint);

    proc_shm_ptr->pid[M30_MANAGER_PID] = getpid();
    proc_shm_ptr->status[M30_MANAGER_PID] = 'S';

    if (logger_init("Logs/M30_Manager_Log", 100) != 0) {
        printf("Logger init failed\n");
    }

    // 장치 목록 초기화
    init_device_list();
    
    pthread_t p_thread;
    if (pthread_create(&p_thread, NULL, do_thread, NULL) != 0) {
        logger_log(LOG_LEVEL_ERROR, "Thread creation failed");
        exit(EXIT_FAILURE);
    }

    logger_log(LOG_LEVEL_INFO, "M30 Manager Start. Devices: %d", g_m30_count);

    while (1) {
        usleep(10000); // 10ms
        nowtime = time(NULL);

        manage_connections(); // 연결되지 않은 장비 재연결 시도
        packet_loop();        // 패킷 수신 및 처리
    }

    return 0;
}