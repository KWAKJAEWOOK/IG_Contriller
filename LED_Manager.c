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

// 그룹 별 dimmer ID 설정 (원형교차로 주행 방향 순서대로 정렬)
int g_dimmer_n[] = { 1, 2, 3, 4, 5 };       // 북->서
int g_dimmer_w[] = { 6, 7, 8, 9, 10 };      // 서->남
int g_dimmer_s[] = { 11, 12, 13, 14, 15 };  // 남->동
int g_dimmer_e[] = { 16, 17, 18, 19, 20 };  // 동->북
const int count_n_group = (sizeof(g_dimmer_n)/sizeof(int));
const int count_e_group = (sizeof(g_dimmer_e)/sizeof(int));
const int count_s_group = (sizeof(g_dimmer_s)/sizeof(int));
const int count_w_group = (sizeof(g_dimmer_w)/sizeof(int));

int* g_dimmer_groups[4] = { g_dimmer_n, g_dimmer_w, g_dimmer_s, g_dimmer_e };   // 그룹 별 Dimmer ID 배열 // todo. 포인터 말고 다른 방법 없나
// const int g_all_dimmer_cnt = count_n_group + count_e_group + count_s_group + count_w_group; // 전체 Dimmer 개수
const int g_dimmer_cnt[4] = { count_n_group, count_w_group, count_s_group, count_e_group };    // 그룹 별 Dimmer 개수

#define G_ALL_DIMMER_CNT 20 // todo. g_all_dimmer_cnt를 그대로 쓸 수가 없었음. 매 지점별로 설치 개수 넣어줘야되나?
int g_dimmer_current_msg[G_ALL_DIMMER_CNT][3]; // 현재 dimmer에 들어간 메시지

typedef struct {
    // 주행 경로 표출 관련
    int wave_count;
    int wave_idx;
    // 페이즈는 g_dimmer_current_msg로 대체

    // 상충 관련
    int blink_count;
    bool blink_phase;
} ANIME_STATUS;
ANIME_STATUS g_led_anim = {0, 0, 0, true};


// ================= Lookup Table 정의 =================
// [진입방향][진출방향][LED그룹]
// 진입/진출 인덱스: 0:북, 1:동, 2:남, 3:서
// LED 그룹 인덱스: 0:N, 1:W, 2:S, 3:E (g_dimmer_groups 순서)
// 값: 1(켜짐), 0(꺼짐)

const bool g_route_table[4][4][4] = {
    {   // HO 북쪽 진입
        {1, 1, 1, 1}, // HO 북쪽 진출
        {1, 1, 1, 0}, // HO 동쪽 진출
        {1, 1, 0, 0}, // HO 남쪽 진출
        {1, 0, 0, 0}  // HO 서쪽 진출
    },
    {   // HO 동쪽 진입
        {0, 0, 0, 1}, // HO 북쪽 진출
        {1, 1, 1, 1}, // HO 동쪽 진출
        {1, 1, 0, 1}, // HO 남쪽 진출
        {1, 0, 0, 1}  // HO 서쪽 진출
    },
    {   // HO 남쪽 진입
        {0, 0, 1, 1}, // HO 북쪽 진출
        {0, 0, 1, 0}, // HO 동쪽 진출
        {1, 1, 1, 1}, // HO 남쪽 진출
        {1, 0, 1, 1}  // HO 서쪽 진출
    },
    {   // HO 서쪽 진입
        {0, 1, 1, 1}, // HO 북쪽 진출
        {0, 1, 1, 0}, // HO 동쪽 진출
        {0, 1, 0, 0}, // HO 남쪽 진출
        {1, 1, 1, 1}  // HO 서쪽 진출
    }
};
int get_dir_index(int dir_code) {   // 방향 코드를 내부 인덱스로 변환함 (g_route_table 쓰기 편하게)
    if (dir_code == 0) return -1;
    if (dir_code == system_set_ptr->n_dir_code) return 0;
    if (dir_code == system_set_ptr->e_dir_code) return 1;
    if (dir_code == system_set_ptr->s_dir_code) return 2;
    if (dir_code == system_set_ptr->w_dir_code) return 3;
    return -1;
}
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
void send_led_packet(const char* data_content) {    // 경관조명에 패킷 전송
    if (!connection_status_ptr->led_conn || HandleIndex == -1) { return; }

    uint8_t packet[64];
    int len = make_led_packet(packet, data_content);

    if (SendBuf(HandleIndex, (char*)packet, len)) {
        logger_log(LOG_LEVEL_DEBUG, "Sent: %s", data_content);
    } else {
        logger_log(LOG_LEVEL_WARN, "Send Fail: %s", data_content);
        CommClose(HandleIndex); // 전송 실패 시 연결 끊음
        HandleIndex = -1;
        connection_status_ptr->led_conn = false;
    }
}

void send_idxset(int dimmer_id, int r, int g, int b) {  // $IDXSET 명령 전송
    int dimmer_idx = dimmer_id - 1;
    if ((g_dimmer_current_msg[dimmer_idx][0] == r) && (g_dimmer_current_msg[dimmer_idx][1] == g) && (g_dimmer_current_msg[dimmer_idx][2] == b)) { return; }

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "IDXSET:%03d%03d%03d%03d", dimmer_id, r, g, b);
    send_led_packet(cmd);

    g_dimmer_current_msg[dimmer_idx][0] = r;
    g_dimmer_current_msg[dimmer_idx][1] = g;
    g_dimmer_current_msg[dimmer_idx][2] = b;
}
void send_led_seen(int seen_no) {   // $SEEN 명령 전송
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "SEEN:%02d", seen_no);
    send_led_packet(cmd);
}
void send_led_clean() {   // $CLEAN 명령 전송
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "CLEAN");
    send_led_packet(cmd);

    for (int dimmer_idx = 0; dimmer_idx < G_ALL_DIMMER_CNT; dimmer_idx++) {    // 현재메시지 안겹치게 초기화
        for (int i = 0; i < 3; i++) {
            if (g_dimmer_current_msg[dimmer_idx][i] != -1) { g_dimmer_current_msg[dimmer_idx][i] = -1; }
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
// ============================ 기능 구현 함수 ============================
void update_animation() {   // 애니메이션을 위한 카운터 업데이트 함수
    int wave_tick = 1;  // todo. speed 값으로 조절 (대푯값 추출 필요)
    int blink_tick = 4; // todo. pet_gap 값으로 조절 (대푯값 추출 필요)

    g_led_anim.wave_count++;
    if (g_led_anim.wave_count >= wave_tick) {
        g_led_anim.wave_count = 0;
        g_led_anim.wave_idx++;
        if (g_led_anim.wave_idx >= G_ALL_DIMMER_CNT) g_led_anim.wave_idx = 0;   // 전체 Dimmer 순회
    }

    g_led_anim.blink_count++;
    if (g_led_anim.blink_count >= blink_tick) {
        g_led_anim.blink_count = 0;
        g_led_anim.blink_phase = !g_led_anim.blink_phase;
    }
}

void process_all_led() { // LED 표출 제어 함수
    if ((!connection_status_ptr->led_conn) || (!connection_status_ptr->ig_server_conn)) return;

    update_animation(); // 애니메이션 카운터 업데이트

    bool have_conflict_grp[4] = { false, false, false, false }; // 상충 표출해야되는 Dimmer 그룹 (초기값은 다 안해줘도 되는걸로)    // todo. 지금은 HO의 진입 방향을 가지고 표출. 추후 ConflictPos 데이터를 응용할 방법을 찾아보는게 좋을듯
    bool is_conflict_active = false;
    if (vms_command_ptr->n_in_msg[0] == 2) { have_conflict_grp[0] = true; is_conflict_active = true; } // 북쪽 상충
    if (vms_command_ptr->w_in_msg[0] == 2) { have_conflict_grp[1] = true; is_conflict_active = true; } // 서쪽 상충
    if (vms_command_ptr->s_in_msg[0] == 2) { have_conflict_grp[2] = true; is_conflict_active = true; } // 남쪽 상충
    if (vms_command_ptr->e_in_msg[0] == 2) { have_conflict_grp[3] = true; is_conflict_active = true; } // 동쪽 상충

    bool have_waypoint_grp[4] = { false, false, false, false };    // 경로 표출해야되는 Dimmer 그룹 (초기값은 다 안해줘도 되는걸로)
    bool is_waypoint_active = false;
    /*
    for (int i = 0; i < vms_command_ptr->ho_count; i++) {   // 경로 표출해줘야되는 HO 순회하면서 have_waypoint_grp 업데이트
        if ((vms_command_ptr->led_msg[i][0] != 0) && (vms_command_ptr->led_msg[i][1] != 0)) { is_waypoint_active = true; }  // HO의 진입, 진출 방향 코드값이 정상적이면
        if (vms_command_ptr->led_msg[i][0] == system_set_ptr->n_dir_code) { // HO 진입이 북쪽방향이고
            if (vms_command_ptr->led_msg[i][1] == system_set_ptr->n_dir_code) { // 진출이 북쪽방향이면 원형교차로를 한바퀴 돈다는 소리겠지
                have_waypoint_grp[0] = true;
                have_waypoint_grp[1] = true;
                have_waypoint_grp[2] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->e_dir_code) {  // 진출이 남쪽방향일 때
                have_waypoint_grp[0] = true;
                have_waypoint_grp[1] = true;
                have_waypoint_grp[2] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->s_dir_code) {
                have_waypoint_grp[0] = true;
                have_waypoint_grp[1] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->w_dir_code) {
                have_waypoint_grp[0] = true;
            } // 다른 경우엔 주행 의도 미공유거나 어쩌구니까 그냥 스킵
        }
        else if (vms_command_ptr->led_msg[i][0] == system_set_ptr->e_dir_code) {    // HO 진입이 동쪽이면
            if (vms_command_ptr->led_msg[i][1] == system_set_ptr->e_dir_code) {
                have_waypoint_grp[0] = true;
                have_waypoint_grp[1] = true;
                have_waypoint_grp[2] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->s_dir_code) {
                have_waypoint_grp[0] = true;
                have_waypoint_grp[1] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->w_dir_code) {
                have_waypoint_grp[0] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->n_dir_code) {
                have_waypoint_grp[3] = true;
            }
        }
        else if (vms_command_ptr->led_msg[i][0] == system_set_ptr->s_dir_code) {    // HO 진입이 남쪽이면
            if (vms_command_ptr->led_msg[i][1] == system_set_ptr->s_dir_code) {
                have_waypoint_grp[0] = true;
                have_waypoint_grp[1] = true;
                have_waypoint_grp[2] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->w_dir_code) {
                have_waypoint_grp[0] = true;
                have_waypoint_grp[2] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->n_dir_code) {
                have_waypoint_grp[2] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->e_dir_code) {
                have_waypoint_grp[2] = true;
            }
        }
        else if (vms_command_ptr->led_msg[i][0] == system_set_ptr->w_dir_code) {    // HO 진입이 서쪽이면
            if (vms_command_ptr->led_msg[i][1] == system_set_ptr->w_dir_code) {
                have_waypoint_grp[0] = true;
                have_waypoint_grp[1] = true;
                have_waypoint_grp[2] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->n_dir_code) {
                have_waypoint_grp[1] = true;
                have_waypoint_grp[2] = true;
                have_waypoint_grp[3] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->e_dir_code) {
                have_waypoint_grp[1] = true;
                have_waypoint_grp[2] = true;
            } else if (vms_command_ptr->led_msg[i][1] == system_set_ptr->s_dir_code) {
                have_waypoint_grp[1] = true;
            }
        }
    }
    */
    for (int i = 0; i < vms_command_ptr->ho_count; i++) {   // 경로 표출하는 부분 룩업테이블로 수정
        int entry_code = vms_command_ptr->led_msg[i][0];
        int exit_code  = vms_command_ptr->led_msg[i][1];
        int entry_idx = get_dir_index(entry_code);  // 방향 코드를 내부 인덱스(0~3)로 변환
        int exit_idx  = get_dir_index(exit_code);
        if (entry_idx != -1 && exit_idx != -1) {    // 유효한 방향이면 테이블 조회
            is_waypoint_active = true;
            for (int grp = 0; grp < 4; grp++) {
                if (g_route_table[entry_idx][exit_idx][grp]) {
                    have_waypoint_grp[grp] = true;
                }
            }
        }
    }

    if (is_conflict_active || is_waypoint_active) { // 표출할 게 있으면
        int global_idx = 0; // 지금 Dimmer 인덱스 (ID랑 인덱스 헷갈리지않게 조심해라제발)
        for (int grp = 0; grp < 4; grp++) { // 그룹 4개 순회
            for (int i = 0; i < g_dimmer_cnt[grp]; i++) {   // 각 그룹별 Dimmer 순회하기
                int dimmer_id = g_dimmer_groups[grp][i];
                if (have_conflict_grp[grp]) {   // 상충 표출해줘야되면
                    if (g_led_anim.blink_phase) {
                        send_idxset(dimmer_id, 255, 0, 0); // 빨간색
                    } else {
                        send_idxset(dimmer_id, 0, 0, 0);   // 검은색
                    }
                }

                else if (have_waypoint_grp[grp]) {  // 주행 경로 표출해줘야되면
                    if (g_led_anim.wave_idx == global_idx) {    // wave_idx 업데이트는 update_animation이 알아서 해줌
                        send_idxset(dimmer_id, 0, 0, 255); // 파랑색
                    } else {
                        send_idxset(dimmer_id, 0, 0, 0);   // 검은색
                    }
                }

                else {  // 나머지는 꺼
                    send_idxset(dimmer_id, 0, 0, 0);
                }
                global_idx++; // 다음 Dimmer로 인덱스 증가 (전체 Dimmer 리스트 중 인덱스)
            }
        }
    } else {    // 경로가 하나도 없으면 클리어시켜서 SEEN 표출
        send_led_clean();
    }
}

void process_background_scene() {   // SEEN 명령 주기적으로 전송해주기
    static time_t last_seen_time = 0;
    if (!connection_status_ptr->led_conn) { return; }
    if (nowtime - last_seen_time >= 5) {    // 5초마다 보내주기
        send_led_seen(9); // Scene 09
        last_seen_time = nowtime;
        // logger_log(LOG_LEVEL_DEBUG, "Sent Background Scene 09");
    }
}
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

    // 스레드 생성 (필요하면 쓰기)
    // pthread_t p_thread;
    // if (pthread_create(&p_thread, NULL, do_thread, NULL) != 0) {
    //     logger_log(LOG_LEVEL_ERROR, "Thread creation failed");
    //     exit(EXIT_FAILURE);
    // }

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
			packet_frame();             // 수신 처리
            process_all_led();   // 표출 로직
            process_background_scene(); // 배경 씬 설정
		}
	}

	if (shm_close() != 0)
	{
		logger_log(LOG_LEVEL_ERROR, "LED_Mgr]  shm_close() failed\n");
		exit(-1);
	}

	return 0;
}