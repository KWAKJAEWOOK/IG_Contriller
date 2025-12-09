/*
    LED_Manager.c
    경관조명 표출 관리 프로세스
    1. 1:1 TCP 소켓 통신, 클라이언트로써 동작
    2. IG_Server_Manager에서 업데이트한 공유메모리 읽기
    3. 적절한 표출 제어
    4. 연결 이상 감지 시 자동 재연결 시도
*/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <stdarg.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"
#include "global/logger.h"

// LED 제어용 전역 변수
time_t nowtime;
time_t last_keep_alive_time;
THANDLEINDEX HandleIndex = -1;
volatile BOOL bConnected = false;

// 수신 버퍼
#define MAX_RECV_BUFFER_SIZE (1024 * 4) 
static uint8_t g_recv_buffer[MAX_RECV_BUFFER_SIZE];
static size_t g_buffer_len = 0;

// dimmer 그룹 설정
int g_dimmer_n[] = { 1, 2, 3, 4, 5 };       // 북->동
int g_dimmer_e[] = { 6, 7, 8, 9, 10 };      // 동->남
int g_dimmer_s[] = { 11, 12, 13, 14, 15 };  // 남->서
int g_dimmer_w[] = { 16, 17, 18, 19, 20 };  // 서->북
const int count_n_group = (sizeof(g_dimmer_n)/sizeof(int));  // 그룹 개수
const int count_e_group = (sizeof(g_dimmer_e)/sizeof(int));
const int count_s_group = (sizeof(g_dimmer_s)/sizeof(int));
const int count_w_group = (sizeof(g_dimmer_w)/sizeof(int));

typedef struct {
    char last_msg[128];
    int last_pet_gap;
} GROUP_STATE;
GROUP_STATE g_state[4]; // n, e, s, w

//========================= 소소한 헬퍼 함수 =========================

void handle_sigint(int sig) {   // Ctrl+C 핸들러
    printf("\nLED_Manager Terminating... (Signal: %d)\n", sig);
    if (shm_close() != 0) printf("shm_close() failed\n");
    if (HandleIndex != -1) CommClose(HandleIndex);
    exit(0);
}


long long current_timestamp_ms() {  // 애니메이션용 타임스탬프 기록
    struct timeval te;
    gettimeofday(&te, NULL);
    return te.tv_sec * 1000LL + te.tv_usec / 1000;
}
// ============================ 연결 관리 ============================
bool host_connect() {
    if (connection_status_ptr->led_conn) { return true; }
    logger_log(LOG_LEVEL_DEBUG, "LED_Mgr] Try to Connect IP:%s, Port:%d", 
               system_set_ptr->led_ip, system_set_ptr->led_port);
    HandleIndex = CommInit(CONNECT, system_set_ptr->led_ip, system_set_ptr->led_port, true);
    if (HandleIndex == -1) {
        logger_log(LOG_LEVEL_ERROR, "LED_Mgr] Connect Fail: IP:%s, Port:%d", 
               system_set_ptr->led_ip, system_set_ptr->led_port);
        connection_status_ptr->led_conn = false;
        return false;
    } else {
        logger_log(LOG_LEVEL_INFO, "LED_Mgr] Connect Success: IP:%s, Port:%d", 
               system_set_ptr->led_ip, system_set_ptr->led_port);
        connection_status_ptr->led_conn = true;
        last_keep_alive_time = time(NULL);
    }
    usleep(100000);   //100ms
    return true;
}

// ============================ 데이터 처리 ============================
#define RECV_BUF_SIZE 2048
uint8_t g_recv_buf[RECV_BUF_SIZE];
size_t g_buf_len = 0;
void packet_frame() {   // 응답 수신
    if (!connection_status_ptr->led_conn || HandleIndex == -1) { return; }
    int nRead = 0;
    size_t space = RECV_BUF_SIZE - g_buf_len;
    if (space == 0) { g_buf_len = 0; space = RECV_BUF_SIZE; } // 버퍼 꽉차면 초기화
    BOOL res = RecvBuf(HandleIndex, (char*)(g_recv_buf + g_buf_len), space, &nRead, 0);

    if (res == TRUE && nRead > 0) {
        if (connection_status_ptr->ig_server_conn == false) { connection_status_ptr->ig_server_conn = true; }	// 데이터 정상 수신 시 통신상태 정상
        last_keep_alive_time = time(NULL);
        g_buf_len += nRead;
        // 필요하면 $OK 등 응답 내용 확인
        g_buf_len = 0; // 그냥 비우기
    } else if (res == FALSE) {
        int err = GetLastCommError();
        if (err == DISCONNECTED || (nRead == 0 && err != TIME_OUT)) {
            logger_log(LOG_LEVEL_WARN, "LED_Mgr] LED Disconnected");
            CommClose(HandleIndex);
            HandleIndex = -1;
            connection_status_ptr->led_conn = false;
        }
    }
}

int make_led_packet(uint8_t *buf, const char *data_str) { // 패킷 헤더랑 \r, \n 추가해주는 헬퍼
    int data_len = strlen(data_str);
    int idx = 0;
    buf[idx++] = 0x24; // $
    memcpy(&buf[idx], data_str, data_len); // Data
    idx += data_len;
    buf[idx++] = 0x0D; // \r
    buf[idx++] = 0x0A; // \n
    return idx; // 총 길이
}
void send_led_packet(GROUP_STATE state, const char* data_content) {    // 경관조명에 패킷 전송
    if (!connection_status_ptr->led_conn || HandleIndex == -1) { return; }
    if (strcmp(state.last_msg, data_content) == 0) {    // 변경된 내용이 없으면 안보냄
        // logger_log(LOG_LEVEL_DEBUG, "skipping msg %s[%s]-> msg: %s = %s", ctx->name, ctx->ip, ctx->last_sent_packet_data, data_content);
        return;
    }
    uint8_t packet[1024];
    int len = make_led_packet(packet, data_content);
    if (SendBuf(HandleIndex, (char*)packet, len)) {
        logger_log(LOG_LEVEL_DEBUG, "Sent %s", data_content);
        strncpy(state.last_msg, data_content, sizeof(state.last_msg));
    } else {
        logger_log(LOG_LEVEL_WARN, "Send Fail %s", data_content);
        CommClose(HandleIndex); // 전송 실패 시 연결 끊음
        HandleIndex = -1;
        connection_status_ptr->led_conn = false;
    }
}
// ============================ 메시지 큐 처리 ============================
// 다른 프로세스(IG_Server 등)에서 보낸 명령 처리
void message_analy(BYTE *_pData) {  // 외부 프로세스에서 받은 데이터 파싱
	if(_pData[0] != (0x80 | LED_MANAGER_Q_PID))
	{
		printf("massege id %02x %02x err~\n", _pData[0], (0x80 | LED_MANAGER_Q_PID));
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

    while (1) {
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
		}
		if(st_5s_cnt++ >= 49)
		{
			st_5s_cnt = 0;
		}


        // 링버퍼(메시지큐 대용) 확인
        recv_size = 0;
        tmp_msg_check = false;
        while (system_set_ptr->msg_LED_Q.head != system_set_ptr->msg_LED_Q.tail) {
            recv_msg[recv_size++] = system_set_ptr->msg_LED_Q.buffer[system_set_ptr->msg_LED_Q.tail];
            system_set_ptr->msg_LED_Q.tail++;
            if (system_set_ptr->msg_LED_Q.tail >= BUF_SIZE)
                system_set_ptr->msg_LED_Q.tail = 0;
            tmp_msg_check = true;
        }
        if (tmp_msg_check) {
            message_analy(recv_msg);
        }
    }
}
// ============================ 기능 구현 함수들 ============================
// todo. LED 전체 기본 표출 씬 패킷 전송 ($SEEN:09)
// todo. dimmer 인덱스를 4개로 나누기 (교차로 진입/진출 도로 사이)
// todo. vms_command_ptr 내부 값을 확인하고, $IDXSET을 순회하며 차량들의 주행 경로를 제외한 LED들의 표출을 검은색으로 설정하는 함수
// todo. vms_command_ptr 내부 값을 확인하고, 상충이 예상되는 지점에 IDXSET으로 붉은색->검은색 점멸 표출 함수 (PET 값이 작으면 작을수록 빠르게 점멸)
// todo. 응답이 없으면 connection_status_ptr 연결상태에 기록하고, 재연결 시도
/*
패킷: $IDXSET:XXXXXXXXXXXX [XXX(dimmer ID: 001~999), XXX(RED: 0~255), XXX(GREEN: 0~255), XXX(BLUE: 0~255)]
    -> ACK: $OK19
    -> 선택되지 않은 Dimmer는 마지막으로 설정된 출력(IDXSET, SEEN, OVSTAEND) 유지
패킷: $CLEAN [전체 Dimmer 에 대하여, IDXSET으로 설정된 출력 설정 초기화]
    -> ACK: $OK19
    -> IDXSET 이전에 설정된 출력(SEEN, OVSTAEND) 유지
*/
// ============================ MAIN ============================
int main() {
    if (logger_init("Logs/LED_Manager_Log", 100) != 0) { // 테스트용 100mb
        printf("Logger init failed\n");
    }
    logger_log(LOG_LEVEL_INFO, "LED Manager Start.");

    if (shm_all_open() == false) {
        logger_log(LOG_LEVEL_ERROR, "LED_Mgr] shm_init failed");
        exit(-1);
    }

    if (msg_all_open() == false) {
        logger_log(LOG_LEVEL_ERROR, "LED_Mgr] msg_init failed");
    }

    signal(SIGINT, handle_sigint);

    // PID 등록
    proc_shm_ptr->pid[LED_MANAGER_PID] = getpid();
    proc_shm_ptr->status[LED_MANAGER_PID] = 'S';

    // 스레드 생성
    pthread_t p_thread;
    if (pthread_create(&p_thread, NULL, do_thread, NULL) != 0) {
        logger_log(LOG_LEVEL_ERROR, "Thread creation failed");
        exit(EXIT_FAILURE);
    }

    nowtime = time(NULL);

    while (1) {
        usleep(50000);  //50ms
        if (connection_status_ptr->led_conn == false) {
            if (HandleIndex != -1) {	// 재연결해야되는데 소켓이 살아있으면
                logger_log(LOG_LEVEL_INFO, "LED_Mgr] Cleaning up old socket handle: %d", HandleIndex);
                CommClose(HandleIndex);
                HandleIndex = -1;
            }
            static time_t last_retry = 0;
            time_t current_time = time(NULL);
			bool Return_re;
            if (current_time - last_retry > 3) { // 3초마다 재시도
                Return_re = host_connect();
				logger_log(LOG_LEVEL_DEBUG, "LED_Mgr] 서버 리스닝 : %d", Return_re);
                last_retry = current_time;
            }
		} else {
			packet_frame();
		}
	}

	if (shm_close() != 0)
	{
		logger_log(LOG_LEVEL_ERROR, "LED_Mgr]  shm_close() failed\n");
		exit(-1);
	}

	return 0;
}