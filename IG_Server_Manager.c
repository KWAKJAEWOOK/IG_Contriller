/*
	IG_Server_Manager.c
    IG-Server 데이터 수신 관리 프로세스
    1. 1:1 TCP 소켓 통신, 클라이언트로써 동작
    2. 데이터를 수신하여 파싱/가공, 공유메모리에 업데이트
    3. 서버와 연결 이상 감지 시, 자동 재연결 시도
*/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"
#include "global/logger.h"
#include "global/cJSON.h"

time_t nowtime;
time_t last_keep_alive_time;
THANDLEINDEX HandleIndex = -1;
volatile BOOL bConnected = false; // 연결 상태 플래그

// Ctrl+C 종료 처리 핸들러
void handle_sigint(int sig) {
    printf("\nIG_Server_Manager Terminating... (Signal: %d)\n", sig);
    if (shm_close() != 0) {
        printf("shm_close() failed\n");
    }
    // 필요 시 소켓 close 추가
    exit(0);
}

//============================ TCP 연결 관리 =============================
bool host_connect() {   // 클라이언트로써 연결 시도
	if (bConnected) { return true; }
	logger_log(LOG_LEVEL_DEBUG, "IG_Server] Try to Connect IP:%s, Port:%d"
            , system_set_ptr->ig_server_ip
            , system_set_ptr->ig_server_port);
	HandleIndex = CommInit(CONNECT, &system_set_ptr->ig_server_ip, system_set_ptr->ig_server_port, true);
	if (HandleIndex == -1) {
		logger_log(LOG_LEVEL_ERROR, "IG_Server] Connect Fail, IP:%s, Port:%d", &system_set_ptr->ig_server_ip, system_set_ptr->ig_server_port);
		return false;
	} else {
		logger_log(LOG_LEVEL_DEBUG, "IG_Server] Connect Success, IP:%s, Port:%d", &system_set_ptr->ig_server_ip, system_set_ptr->ig_server_port);
		bConnected = true;
		last_keep_alive_time = time(NULL);
    }
	usleep(100000);  //100ms
	return true;
}
//============================ TCP 수신 함수 =============================
#define MAX_RECV_BUFFER_SIZE (1024 * 16)	// 한번에 최대 16kb
static uint8_t g_recv_buffer[MAX_RECV_BUFFER_SIZE];	// 글로벌 버퍼
static size_t g_buffer_len = 0;

static void Analysis_Packet(cJSON* json_root) {	// IG-Server에서 받은 cJSON 객체 파싱 및 공유메모리에 업데이트
    const cJSON* MsgCount = cJSON_GetObjectItemCaseSensitive(json_root, "MsgCount");
	// todo. 이후 내용 파싱 및 공유메모리 업데이트
}

void append_to_global_buffer(uint8_t* new_data, size_t length) {	// 수신한 데이터를 글로벌 버퍼에 업로드
    if (g_buffer_len + length > MAX_RECV_BUFFER_SIZE) {
        printf("SAFTYCON] 전역 버퍼 오버플로우: 버퍼 초기화.\n");
        g_buffer_len = 0;
        return;
    }
    memcpy(&g_recv_buffer[g_buffer_len], new_data, length);
    g_buffer_len += length;
}
void remove_from_global_buffer(size_t remove_len) {	// 글로벌 버퍼에서 데이터 삭제
    if (remove_len == 0) return;
    if (remove_len >= g_buffer_len) {
        g_buffer_len = 0;
    } else {
        size_t remaining = g_buffer_len - remove_len;
        memmove(&g_recv_buffer[0], &g_recv_buffer[remove_len], remaining);
        g_buffer_len = remaining;
    }
}
void process_parsing() {    // 글로벌 버퍼에서 데이터 파싱
    while (g_buffer_len >= 13) {
        if (g_recv_buffer[5] != 0x4D || g_recv_buffer[6] != 0x73 ||
            g_recv_buffer[7] != 0x67 || g_recv_buffer[8] != 0x43 ||
            g_recv_buffer[9] != 0x6F || g_recv_buffer[10] != 0x75 ||
            g_recv_buffer[11] != 0x6E || g_recv_buffer[12] != 0x74) { // JSON 내용 중 "MsgCount" 위치 확인
            remove_from_global_buffer(1);   // 못찾았으면 1바이트만 버리고 다음 루프에서 재검사
            continue;
        } else {    // 헤더 찾음
            uint32_t msg_len;
            memcpy(&msg_len, &g_recv_buffer[0], sizeof(msg_len));
			// msg_len = ntohl(msg_len);	// 빅엔디안이라면
            if (g_buffer_len < msg_len + 4) { break; }  // 길이만큼 안왔으면 다음 수신 기다리기
            cJSON* json_root = cJSON_ParseWithLength((const char*)&g_recv_buffer[4], msg_len);
            if (json_root) {	// JSON 파싱에 성공했을 시
                Analysis_Packet(json_root);
				/* 디버깅용 수신 JSON 데이터 로깅 */
                char *json_string = cJSON_PrintUnformatted(json_root);
                if (json_string != NULL) {
                    logger_log(LOG_LEVEL_DEBUG, "수신 JSON 객체:\n%s\n", json_string);
                    cJSON_free(json_string);
                }
                cJSON_Delete(json_root);
				//==============================//
            } else {
                logger_log(LOG_LEVEL_ERROR, "JSON 파싱 오류. (len=%zu)\n", msg_len);
            }
            remove_from_global_buffer(msg_len + 4); // 데이터+길이만큼 글로벌 버퍼에서 삭제

        }
    }
}
void packet_frame() {
	int nReadSize = 0;
    BOOL bReturn;
	if (g_buffer_len >= MAX_RECV_BUFFER_SIZE) {
        printf("IG_Server] 수신 버퍼 오버플로우. 버퍼 리셋.\n");
        logger_log(LOG_LEVEL_ERROR, "Receive buffer overflow. Resetting buffer.");
        g_buffer_len = 0;
    }
	size_t free_space = MAX_RECV_BUFFER_SIZE - g_buffer_len;
	if (free_space == 0) {	// 버퍼에 남은 공간 없음
        return;
    }
    if (HandleIndex != -1) {
        bReturn = RecvBuf(HandleIndex, (char *)(g_recv_buffer + g_buffer_len), free_space, &nReadSize, 0);
    }

    if (bReturn == TRUE && nReadSize > 0) {
		if (connection_status_ptr->ig_server_conn == false) { connection_status_ptr->ig_server_conn = true; }	// 데이터 정상 수신 시 통신상태 정상
		g_buffer_len += nReadSize;
		last_keep_alive_time = time(NULL);
		// 디버깅용 출력
        // printf("[RX %lu bytes] ", nReadSize);
        // for (int i = 0; i < nReadSize; i++) {
        //     printf("%02X ", buffer[i]);
        // }
        // printf("\n");
    } else if (bReturn == FALSE) {
        int err = GetLastCommError(); // CommData.c의 에러코드 확인
        // 연결이 끊어진 경우 (ReadnStream에서 0 리턴 시 DISCONNECTED 설정됨)
        if (err == DISCONNECTED || (nReadSize == 0 && err != TIME_OUT)) {
            logger_log(LOG_LEVEL_INFO, "IG_Server] Disconnected by Server/Network.");
            CommClose(HandleIndex);
            HandleIndex = -1;
            bConnected = FALSE;
        }
	}
	process_parsing();  // 글로벌 버퍼에 쌓인 데이터 파싱 시도
}
//================================================================


//===================== 프로세스간 메시지 송/수신 =====================
void message_analy(BYTE *_pData) {  // 외부 프로세스에서 받은 데이터 파싱
	if(_pData[0] != (0x80 | IG_SERVER_Q_PID))
	{
		printf("massege id %02x %02x err~\n", _pData[0], (0x80 | IG_SERVER_Q_PID));
		return;
	}
	switch(_pData[1]) //opcode
	{
		case 0x40: {	// todo. 추가 예정?
            break;
        }
    }
}

void massge_send(BYTE _sel, BYTE *_pada, int _size)	{   // 다른 프로세스에 메세지 전송하는 함수
	for(int i=0;i<_size;i++)
	{
		switch(_sel)
		{
			case LED_MANAGER_Q_PID:
				system_set_ptr->msg_LED_Q.buffer[system_set_ptr->msg_LED_Q.head++] = _pada[i];
				if(system_set_ptr->msg_LED_Q.head >= BUF_SIZE)
					system_set_ptr->msg_LED_Q.head = 0;
				break;
			case M30_MANAGER_Q_PID:
				system_set_ptr->msg_M30_Q.buffer[system_set_ptr->msg_M30_Q.head++] = _pada[i];
				if(system_set_ptr->msg_M30_Q.head >= BUF_SIZE)
					system_set_ptr->msg_M30_Q.head = 0;
				break;
			case CONNECTION_MANAGER_Q_PID:
				system_set_ptr->msg_Connection_manager_Q.buffer[system_set_ptr->msg_Connection_manager_Q.head++] = _pada[i];
				if(system_set_ptr->msg_Connection_manager_Q.head >= BUF_SIZE)
					system_set_ptr->msg_Connection_manager_Q.head = 0;
				break;
		}
	}
}
//=====================================================================

void *do_thread(void * data)	// 100ms 주기로 공유메모리 수신 / 송신
{
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
			if ((nowtime-last_keep_alive_time) >= 3) {	// 3초 이상 데이터가 안 들어오면
				logger_log(LOG_LEVEL_ERROR, "3초 이상 수신 데이터 없음. 소켓 해제 및 재연결 시도");
				close(HandleIndex);
				HandleIndex = -1;
				bConnected = FALSE;
			}
		}
		if(st_5s_cnt++ >= 49)
		{
			st_5s_cnt = 0;
		}

		//메시지 큐 대용
		recv_size = 0;
		tmp_msg_check = false;
		while(system_set_ptr->msg_IG_Server_Q.head != system_set_ptr->msg_IG_Server_Q.tail)
		{
			recv_msg[recv_size++] = system_set_ptr->msg_IG_Server_Q.buffer[system_set_ptr->msg_IG_Server_Q.tail];
			system_set_ptr->msg_IG_Server_Q.tail++;
			if(system_set_ptr->msg_IG_Server_Q.tail >= BUF_SIZE)
				system_set_ptr->msg_IG_Server_Q.tail=0;
			tmp_msg_check = true;
		}
		if(tmp_msg_check)  //여기서 분석 및 응답
		{
			// printf("ig_server message %d : \n", recv_size);
			// for(int i=0;i<recv_size;i++)
			// 	printf("%02x ",recv_msg[i]);
			// printf("\n");
			message_analy(recv_msg);
		}
	}
}
// ================================ 기능 구현 함수 ===============================
// todo. 수신한 JSON 객체를 파싱하여 message_data_ptr에 저장해두는 함수
// todo. 객체 별 교차로 진입 방향 및 진출 방향을 추정하고, 중요 내용들을 vms_command_ptr에 업데이트
// ============================================================================
int main()
{
	int i;
	bool bReturn;

	if (shm_all_open() == false)
	{
		logger_log(LOG_LEVEL_ERROR, "IG_Server] shm_init() failed\n");
		exit(-1);
	}

	if (msg_all_open() == false)
	{
		logger_log(LOG_LEVEL_ERROR, "IG_Server] msg_all_open() failed\n");
		exit(-1);
	}

	signal(SIGINT, handle_sigint);

	proc_shm_ptr->pid[IG_SERVER_MANAGER_PID] = getpid();
	proc_shm_ptr->status[IG_SERVER_MANAGER_PID] = 'S';

	// message queue clear
	msg_clear(IG_SERVER_Q);

	int thr_id; //, status;
	int res;
	void *status;
	pthread_t p_thread;
	if (pthread_create(&p_thread, NULL, do_thread, NULL) != 0) {
		logger_log(LOG_LEVEL_ERROR, "Thread creation failed");
		exit(EXIT_FAILURE);
	}

	if (logger_init("Logs/IG_Server_Manager_Log", 100) != 0) {	// 로거 테스트용 100mb
		logger_log(LOG_LEVEL_ERROR, "Logger init failed");
        exit(EXIT_FAILURE);
    }
	logger_log(LOG_LEVEL_INFO, "IG-Server Manager Start.");

	bReturn = host_connect();	// IG-Server와 연결
	logger_log(LOG_LEVEL_DEBUG, "IG_Server] 서버 리스닝 : %d", bReturn);

	nowtime = time(NULL);	// 재연결 타임아웃 검지를 위한 현재시간 초기화

	while (1)
	{
		usleep(50000);  //50ms
		if(bConnected == false) {
			connection_status_ptr->ig_server_conn = false;
            static time_t last_retry = 0;
            time_t current_time = time(NULL);
            if (current_time - last_retry > 3) { // 3초마다 재시도
                printf("IG_Server] Retrying connection...\n");
                host_connect();
                last_retry = current_time;
            }
		} else {
			packet_frame();
		}
	}

	if (shm_close() != 0)
	{
		logger_log(LOG_LEVEL_ERROR, "IG_Server]  shm_close() failed\n");
		exit(-1);
	}

	return 0;
}
