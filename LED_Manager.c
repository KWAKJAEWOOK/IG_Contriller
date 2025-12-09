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

// Ctrl+C 핸들러
void handle_sigint(int sig) {
    printf("\nLED_Manager Terminating... (Signal: %d)\n", sig);
    if (shm_close() != 0) printf("shm_close() failed\n");
    if (HandleIndex != -1) CommClose(HandleIndex);
    exit(0);
}

// ============================ 연결 관리 ============================
bool host_connect() {
    if (bConnected) return true;
    // 공유메모리 설정값 확인
    if (strlen(system_set_ptr->led_ip) == 0 || system_set_ptr->led_port == 0) {
        // IP 설정이 아직 안되었거나 비어있으면 대기
        return false; 
    }
    logger_log(LOG_LEVEL_DEBUG, "LED_Mgr] Try to Connect IP:%s, Port:%d", 
               system_set_ptr->led_ip, system_set_ptr->led_port);
    HandleIndex = CommInit(CONNECT, system_set_ptr->led_ip, system_set_ptr->led_port, true);
    if (HandleIndex == -1) {
        logger_log(LOG_LEVEL_ERROR, "LED_Mgr] Connect Fail");
        return false;
    } else {
        logger_log(LOG_LEVEL_INFO, "LED_Mgr] Connect Success");
        bConnected = true;
        last_keep_alive_time = time(NULL);
    }
    usleep(100000); 
    return true;
}

// ============================ 데이터 처리 ============================
// LED 장비 프로토콜에 맞게 파싱 로직 구현 필요
void process_parsing() {
    // 예시: 간단히 버퍼 비우기 (실제 구현 시 프로토콜에 맞춰 수정)
    if (g_buffer_len > 0) {
        // printf("LED Recv: %d bytes\n", (int)g_buffer_len);
        g_buffer_len = 0; // 데이터 처리 완료 가정
    }
}

void packet_frame() {
    int nReadSize = 0;
    BOOL bReturn;
    size_t free_space = MAX_RECV_BUFFER_SIZE - g_buffer_len;
    if (free_space == 0) {
        g_buffer_len = 0; // Overflow 방지 리셋
        free_space = MAX_RECV_BUFFER_SIZE;
    }
    if (HandleIndex != -1) {
        // 논블로킹 수신 (WaitSec = 0)
        bReturn = RecvBuf(HandleIndex, (char *)(g_recv_buffer + g_buffer_len), free_space, &nReadSize, 0);
    }
    if (bReturn == TRUE && nReadSize > 0) {
        g_buffer_len += nReadSize;
        last_keep_alive_time = time(NULL);
        process_parsing();
    } else if (bReturn == FALSE) {
        int err = GetLastCommError();
        if (err == DISCONNECTED || (nReadSize == 0 && err != TIME_OUT)) {
            logger_log(LOG_LEVEL_WARN, "LED_Mgr] Disconnected.");
            CommClose(HandleIndex);
            HandleIndex = -1;
            bConnected = FALSE;
        }
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
        usleep(10000); // 10ms

        if (bConnected == false) {
            static time_t last_retry = 0;
            if (nowtime - last_retry > 3) { // 너무 자주 연결 시도 안하기
                host_connect();
                last_retry = nowtime;
            }
        } else {
            packet_frame();
        }
    }

    return 0;
}