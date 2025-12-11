/*
    Connection_Manager.c
    TCP 소켓 연결 및 상태 관리 프로세스
    1. 공유메모리/메시지큐 연결
    2. 주기적으로 각 장치(IG_Server, LED, M30)의 연결 상태 모니터링
    3. (추후 구현) 센터와 연계하여 상태정보 전달
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"
#include "global/logger.h"
#include "global/cJSON.h"

time_t nowtime;
time_t last_keep_alive_time;
THANDLEINDEX HandleIndex = -1;
volatile BOOL bConnected = false;

enum {  // 센터 전송용 각 장치별 아이디
    E1,
    E2,
    E3
};

//========================== 소소한 어쩌구 ========================
void handle_sigint(int sig) {   // Ctrl+C 종료 핸들러
    printf("\nConnection_Manager Terminating... (Signal: %d)\n", sig);
    if (shm_close() != 0) {
        printf("shm_close() failed\n");
    }
    exit(0);
}
//==================================================================
bool host_connect() {   // 클라이언트로써 동작  // todo. 변수들 변경이나 추가해야함
    if (connection_status_ptr->led_conn) { return true; }
    logger_log(LOG_LEVEL_DEBUG, "Conn_Mgr] Try to Connect IP:%s, Port:%d", 
               system_set_ptr->led_ip, system_set_ptr->led_port);
    HandleIndex = CommInit(CONNECT, system_set_ptr->led_ip, system_set_ptr->led_port, true);
    if (HandleIndex == -1) {
        logger_log(LOG_LEVEL_ERROR, "Conn_Mgr] Connect Fail: IP:%s, Port:%d", 
               system_set_ptr->led_ip, system_set_ptr->led_port);
        connection_status_ptr->led_conn = false;
        return false;
    } else {
        logger_log(LOG_LEVEL_INFO, "Conn_Mgr] Connect Success: IP:%s, Port:%d", 
               system_set_ptr->led_ip, system_set_ptr->led_port);
        connection_status_ptr->led_conn = true;
        last_keep_alive_time = time(NULL);
    }
    usleep(100000);   //100ms
    return true;
}

bool host_listen(void) {    // 서버로 동작
    short port = 6003;
    logger_log(LOG_LEVEL_INFO, "Conn_Mgr] Try to Listen Port:%d\n", port);
    HandleIndex = CommInit(ACCEPT, NULL, port, true);  // 디버그 출력 on
    if (HandleIndex == -1) {
        logger_log(LOG_LEVEL_INFO, "Conn_Mgr] CommInit(ACCEPT) Fail, Port %d\n", port);
        return false;
    }
    // 2) 블로킹으로 accept (WAIT: 무한대기)
    if (!CommAccept(HandleIndex, WAIT)) {
        logger_log(LOG_LEVEL_INFO, "Conn_Mgr] CommAccept() Fail, Port %d  (TIME_OUT/ACCEPT_FAIL 등)\n", port);
        return false;
    }
    logger_log(LOG_LEVEL_INFO, "Conn_Mgr] CommAccept() Success, Port %d\n", port);
    return true;
}

// ============================ 메시지 큐 처리 ============================
// 다른 프로세스(IG_Server 등)에서 보낸 명령 처리
void message_analy(BYTE *_pData) {  // 외부 프로세스에서 받은 데이터 파싱
	if(_pData[0] != (0x80 | CONNECTION_MANAGER_Q_PID))
	{
		printf("massege id %02x %02x err~\n", _pData[0], (0x80 | CONNECTION_MANAGER_Q_PID));
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
            logging_status_info();
		}
		if(st_5s_cnt++ >= 49)
		{
			st_5s_cnt = 0;
		}

        // 링버퍼(메시지큐 대용) 확인
        recv_size = 0;
        tmp_msg_check = false;
        while (system_set_ptr->msg_Connection_manager_Q.head != system_set_ptr->msg_Connection_manager_Q.tail) {
            recv_msg[recv_size++] = system_set_ptr->msg_Connection_manager_Q.buffer[system_set_ptr->msg_Connection_manager_Q.tail];
            system_set_ptr->msg_Connection_manager_Q.tail++;
            if (system_set_ptr->msg_Connection_manager_Q.tail >= BUF_SIZE)
                system_set_ptr->msg_Connection_manager_Q.tail = 0;
            tmp_msg_check = true;
        }
        if (tmp_msg_check) {
            message_analy(recv_msg);
        }
    }
}
// ============================ 기능 구현 함수 ============================
void logging_status_info() {    // 디버깅용 상태정보 로깅
    logger_log(LOG_LEVEL_DEBUG, "장치 통신상태 로깅\n"
                                "   IG-Server: %d\n"
                                "   경관조명: %d\n"
                                "   표출장치\n"
                                "      북쪽 진입 볼라드: %d %d %d %d %d\n"
                                "      북쪽 도로 볼라드: %d %d %d %d %d\n"
                                "      북쪽 진출 볼라드: %d %d %d %d %d\n\n"
                                "      동쪽 진입 볼라드: %d %d %d %d %d\n"
                                "      동쪽 도로 볼라드: %d %d %d %d %d\n"
                                "      동쪽 진출 볼라드: %d %d %d %d %d\n\n"
                                "      남쪽 진입 볼라드: %d %d %d %d %d\n"
                                "      남쪽 도로 볼라드: %d %d %d %d %d\n"
                                "      남쪽 진출 볼라드: %d %d %d %d %d\n\n"
                                "      서쪽 진입 볼라드: %d %d %d %d %d\n"
                                "      서쪽 도로 볼라드: %d %d %d %d %d\n"
                                "      서쪽 진출 볼라드: %d %d %d %d %d\n\n"
                            , connection_status_ptr->ig_server_conn
                            , connection_status_ptr->led_conn
                            , connection_status_ptr->m30_n_in_comm[0], connection_status_ptr->m30_n_in_comm[1], connection_status_ptr->m30_n_in_comm[2], connection_status_ptr->m30_n_in_comm[3], connection_status_ptr->m30_n_in_comm[4]
                            , connection_status_ptr->m30_n_load_comm[0], connection_status_ptr->m30_n_load_comm[1], connection_status_ptr->m30_n_load_comm[2], connection_status_ptr->m30_n_load_comm[3], connection_status_ptr->m30_n_load_comm[4]
                            , connection_status_ptr->m30_n_out_comm[0], connection_status_ptr->m30_n_out_comm[1], connection_status_ptr->m30_n_out_comm[2], connection_status_ptr->m30_n_out_comm[3], connection_status_ptr->m30_n_out_comm[4]
                        
                            , connection_status_ptr->m30_e_in_comm[0], connection_status_ptr->m30_e_in_comm[1], connection_status_ptr->m30_e_in_comm[2], connection_status_ptr->m30_e_in_comm[3], connection_status_ptr->m30_e_in_comm[4]
                            , connection_status_ptr->m30_e_load_comm[0], connection_status_ptr->m30_e_load_comm[1], connection_status_ptr->m30_e_load_comm[2], connection_status_ptr->m30_e_load_comm[3], connection_status_ptr->m30_e_load_comm[4]
                            , connection_status_ptr->m30_e_out_comm[0], connection_status_ptr->m30_e_out_comm[1], connection_status_ptr->m30_e_out_comm[2], connection_status_ptr->m30_e_out_comm[3], connection_status_ptr->m30_e_out_comm[4]

                            , connection_status_ptr->m30_s_in_comm[0], connection_status_ptr->m30_s_in_comm[1], connection_status_ptr->m30_s_in_comm[2], connection_status_ptr->m30_s_in_comm[3], connection_status_ptr->m30_s_in_comm[4]
                            , connection_status_ptr->m30_s_load_comm[0], connection_status_ptr->m30_s_load_comm[1], connection_status_ptr->m30_s_load_comm[2], connection_status_ptr->m30_s_load_comm[3], connection_status_ptr->m30_s_load_comm[4]
                            , connection_status_ptr->m30_s_out_comm[0], connection_status_ptr->m30_s_out_comm[1], connection_status_ptr->m30_s_out_comm[2], connection_status_ptr->m30_s_out_comm[3], connection_status_ptr->m30_s_out_comm[4]

                            , connection_status_ptr->m30_w_in_comm[0], connection_status_ptr->m30_w_in_comm[1], connection_status_ptr->m30_w_in_comm[2], connection_status_ptr->m30_w_in_comm[3], connection_status_ptr->m30_w_in_comm[4]
                            , connection_status_ptr->m30_w_load_comm[0], connection_status_ptr->m30_w_load_comm[1], connection_status_ptr->m30_w_load_comm[2], connection_status_ptr->m30_w_load_comm[3], connection_status_ptr->m30_w_load_comm[4]
                            , connection_status_ptr->m30_w_out_comm[0], connection_status_ptr->m30_w_out_comm[1], connection_status_ptr->m30_w_out_comm[2], connection_status_ptr->m30_w_out_comm[3], connection_status_ptr->m30_w_out_comm[4]
                        );
}

// static cJSON* create_status_info() { // 공유메모리 읽어서 상태정보로 만들기
//     cJSON* root = cJSON_CreateObject();
//     cJSON* ConnErrDeviceinfoList = cJSON_CreateArray(); // 통신 장애 발생한 디바이스 ID 리스트
//     // todo.
//     cJSON_AddStringToObject(root, "Timestamp", vms_command_ptr->Timestamp);
// }
// ============================ MAIN ============================
int main() {
    if (logger_init("Logs/Connection_Manager_Log", 100) != 0) { // 테스트용 100mb
        printf("Logger init failed\n");
    }
    logger_log(LOG_LEVEL_INFO, "Connection Manager Start.");

    if (shm_all_open() == false) {
        logger_log(LOG_LEVEL_ERROR, "Conn_Mgr] shm_init failed");
        exit(-1);
    }

    if (msg_all_open() == false) {
        logger_log(LOG_LEVEL_ERROR, "Conn_Mgr] msg_init failed");
    }

    signal(SIGINT, handle_sigint);

    // PID 등록
    proc_shm_ptr->pid[CONNECTION_MANAGER_PID] = getpid();
    proc_shm_ptr->status[CONNECTION_MANAGER_PID] = 'S';

    // 스레드 생성
    pthread_t p_thread;
    if (pthread_create(&p_thread, NULL, do_thread, NULL) != 0) {
        logger_log(LOG_LEVEL_ERROR, "Thread creation failed");
        exit(EXIT_FAILURE);
    }

    nowtime = time(NULL);

    while (1) {
        usleep(50000);  //50ms
        // if (connection_status_ptr->led_conn == false) {
        //     if (HandleIndex != -1) {	// 재연결해야되는데 소켓이 살아있으면
        //         logger_log(LOG_LEVEL_INFO, "Conn_Mgr] Cleaning up old socket handle: %d", HandleIndex);
        //         CommClose(HandleIndex);
        //         HandleIndex = -1;
        //     }
        //     static time_t last_retry = 0;
        //     time_t current_time = time(NULL);
		// 	bool Return_re;
        //     if (current_time - last_retry > 3) { // 3초마다 재시도
        //         Return_re = host_listen();  // 이거 이렇게 둬도 되는걸까
		// 		logger_log(LOG_LEVEL_DEBUG, "Conn_Mgr] 소켓 연결 성공 여부 : %d", Return_re);
        //         last_retry = current_time;
        //     }
		// } else {
		// 	packet_frame(); // 수신 처리
		// }
	}

	if (shm_close() != 0)
	{
		logger_log(LOG_LEVEL_ERROR, "LED_Mgr]  shm_close() failed\n");
		exit(-1);
	}

	return 0;
}