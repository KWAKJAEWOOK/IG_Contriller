/*
    M30_Manager.c
    볼라드(M30) 장치 제어 및 상태 관리 프로세스
    - 다중 클라이언트 TCP 연결 관리
    - 공유메모리 명령에 따른 표출 제어 (순차 점등, 점멸 등)
    - 조도 센서 연동 밝기 제어

    todo. IG-Server와 연결 끊기면 0번 인덱스의 메시지로 M30 전체 전송
    todo. 장치 랜 케이블 연결이 끊기거나 흔들릴 시 재연결하는지 테스트
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
#include <sys/time.h>
#include <stdarg.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"
#include "global/logger.h"

#define MAX_M30_DEVICES 64
#define RECV_BUF_SIZE 2048

// 그룹 정의
enum {
    GRP_N_IN = 0, GRP_N_LOAD, GRP_N_OUT,
    GRP_E_IN, GRP_E_LOAD, GRP_E_OUT,
    GRP_S_IN, GRP_S_LOAD, GRP_S_OUT,
    GRP_W_IN, GRP_W_LOAD, GRP_W_OUT,
    TOTAL_GROUPS
};

// M30 장치 컨텍스트
typedef struct {
    char ip[32];
    int port;
    int group_id;       // 위 enum 값
    int group_idx;      // 그룹 내 순서 (0 ~ 4)
    char name[32];      // 로그용 이름

    THANDLEINDEX handle;
    bool connected;

    uint8_t recv_buf[RECV_BUF_SIZE];
    size_t buf_len;

    // 상태 관리용
    char last_sent_packet_data[1024]; // 중복 전송 방지용
    int last_brightness;              // 마지막으로 설정한 밝기
} M30_CONTEXT;

M30_CONTEXT g_m30_devs[MAX_M30_DEVICES];
int g_m30_count = 0;
int g_group_dev_cnt[TOTAL_GROUPS] = {0,};
M30_CONTEXT* g_group_map[TOTAL_GROUPS][5]; // [그룹ID][인덱스]

typedef struct {    // 애니메이션 스레드 딜레이를 위한 구조체
    // 순차 표출
    int wave_count; // 틱에 따른 카운터
    int wave_idx;   // 다음에 바꿔야될 장치 인덱스
    bool wave_phase;   // 꺼야될지 켜야될지 정함

    // 상충 표출
    int blink_count;    // 틱에 따른 카운터
    bool blink_phase;   // 꺼야될지 켜야될지 정함
} ANIME_STATUS;
ANIME_STATUS g_group_anim[TOTAL_GROUPS];

time_t nowtime;
struct timeval tv_now;


// ============================ 유틸리티 함수 ============================
long long current_timestamp_ms() {  // 애니메이션용 타임스탬프 기록
    struct timeval te;
    gettimeofday(&te, NULL);
    return te.tv_sec * 1000LL + te.tv_usec / 1000;
}

uint8_t calc_checksum(uint8_t *packet, int len) {   // Checksum 계산
    uint8_t sum = 0;
    for (int i = 0; i < len - 2; i++) {
        sum += packet[i];
    }
    return sum;
}

int make_m30_packet(uint8_t *buf, uint8_t type, const char *data_str) { // 패킷 생성 헬퍼
    int data_len = strlen(data_str);
    int idx = 0;
    buf[idx++] = 0x02; // STX
    buf[idx++] = type; // Type
    buf[idx++] = (uint8_t)(data_len & 0xFF); // Length Low
    buf[idx++] = (uint8_t)((data_len >> 8) & 0xFF); // Length High
    memcpy(&buf[idx], data_str, data_len); // Data
    idx += data_len;
    buf[idx] = calc_checksum(buf, idx + 1); // 임시 계산 (CS 자리 포함 전까지 더함)
    idx++;
    buf[idx++] = 0x03; // ETX
    return idx; // 총 길이
}

void handle_sigint(int sig) {   // Ctrl+C 핸들러
    printf("\nM30_Manager Terminating... (Signal: %d)\n", sig);
    for(int i=0; i<g_m30_count; i++) {
        if(g_m30_devs[i].handle != -1) CommClose(g_m30_devs[i].handle);
    }
    if (shm_close() != 0) printf("shm_close() failed\n");
    exit(0);
}

// ============================ 초기화 ============================
void register_device(const char* ip, int port, int grp_id, int idx_in_grp, const char* name_prefix) {
    if (strlen(ip) < 7) return; // 이상한 IP
    if (g_m30_count >= MAX_M30_DEVICES) return;

    M30_CONTEXT *ctx = &g_m30_devs[g_m30_count];
    strncpy(ctx->ip, ip, 32);
    ctx->port = port;
    ctx->group_id = grp_id;
    ctx->group_idx = idx_in_grp;
    snprintf(ctx->name, 32, "%s_%d", name_prefix, idx_in_grp + 1);
    
    ctx->handle = -1;
    ctx->connected = false;
    ctx->buf_len = 0;
    memset(ctx->last_sent_packet_data, 0, sizeof(ctx->last_sent_packet_data));
    ctx->last_brightness = -1;

    // 매핑 테이블 업데이트
    g_group_map[grp_id][idx_in_grp] = ctx;
    g_group_dev_cnt[grp_id]++;
    g_m30_count++;
}

void init_device_list() {
    g_m30_count = 0;
    int port = system_set_ptr->m30_port;
    if(port <= 0) port = 7531;

    memset(g_group_map, 0, sizeof(g_group_map));
    memset(g_group_anim, 0, sizeof(g_group_anim));

    // 북쪽
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_n_in_ip[i], port, GRP_N_IN, i, "N_IN");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_n_load_ip[i], port, GRP_N_LOAD, i, "N_LOAD");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_n_out_ip[i], port, GRP_N_OUT, i, "N_OUT");
    
    // 동쪽
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_e_in_ip[i], port, GRP_E_IN, i, "E_IN");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_e_load_ip[i], port, GRP_E_LOAD, i, "E_LOAD");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_e_out_ip[i], port, GRP_E_OUT, i, "E_OUT");

    // 남쪽
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_s_in_ip[i], port, GRP_S_IN, i, "S_IN");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_s_load_ip[i], port, GRP_S_LOAD, i, "S_LOAD");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_s_out_ip[i], port, GRP_S_OUT, i, "S_OUT");

    // 서쪽
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_w_in_ip[i], port, GRP_W_IN, i, "W_IN");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_w_load_ip[i], port, GRP_W_LOAD, i, "W_LOAD");
    for(int i=0; i<5; i++) register_device(system_set_ptr->m30_w_out_ip[i], port, GRP_W_OUT, i, "W_OUT");

    logger_log(LOG_LEVEL_INFO, "M30 Device List Initialized. Total: %d", g_m30_count);
}

// ============================ 연결 관리 ============================
void update_conn_status(M30_CONTEXT *ctx, bool status) {    // 연결 상태를 공유 메모리에 업데이트
    switch(ctx->group_id) {
        case GRP_N_IN: connection_status_ptr->m30_n_in_comm[ctx->group_idx] = status; break;
        case GRP_N_LOAD: connection_status_ptr->m30_n_load_comm[ctx->group_idx] = status; break;
        case GRP_N_OUT: connection_status_ptr->m30_n_out_comm[ctx->group_idx] = status; break;
        
        case GRP_E_IN: connection_status_ptr->m30_e_in_comm[ctx->group_idx] = status; break;
        case GRP_E_LOAD: connection_status_ptr->m30_e_load_comm[ctx->group_idx] = status; break;
        case GRP_E_OUT: connection_status_ptr->m30_e_out_comm[ctx->group_idx] = status; break;

        case GRP_S_IN: connection_status_ptr->m30_s_in_comm[ctx->group_idx] = status; break;
        case GRP_S_LOAD: connection_status_ptr->m30_s_load_comm[ctx->group_idx] = status; break;
        case GRP_S_OUT: connection_status_ptr->m30_s_out_comm[ctx->group_idx] = status; break;

        case GRP_W_IN: connection_status_ptr->m30_w_in_comm[ctx->group_idx] = status; break;
        case GRP_W_LOAD: connection_status_ptr->m30_w_load_comm[ctx->group_idx] = status; break;
        case GRP_W_OUT: connection_status_ptr->m30_w_out_comm[ctx->group_idx] = status; break;
    }
}

void manage_connections() { // 연결 시도 함수
    for (int i = 0; i < g_m30_count; i++) {
        M30_CONTEXT *ctx = &g_m30_devs[i];

        if (ctx->connected) {
            update_conn_status(ctx, true);
            continue;
        }
        update_conn_status(ctx, false);
        
        // 연결 시도 (blocking Connect)
        ctx->handle = CommInit(CONNECT, ctx->ip, ctx->port, true); 
        
        if (ctx->handle != -1) {
            logger_log(LOG_LEVEL_INFO, "M30 Connected: %s (%s)", ctx->name, ctx->ip);
            ctx->connected = true;
            ctx->buf_len = 0;
            // 재연결 시 상태 초기화 (즉시 패킷 전송하도록)
            memset(ctx->last_sent_packet_data, 0, sizeof(ctx->last_sent_packet_data)); 
            ctx->last_brightness = -1; 
        } else {
            logger_log(LOG_LEVEL_INFO, "M30 Connection Failed: %s (%s)", ctx->name, ctx->ip);
        }
    }
}
// ============================ 패킷 전송 ============================
void send_m30_data_packet(M30_CONTEXT *ctx, const char* data_content) {
    if (!ctx->connected || ctx->handle == -1) return;
    if (strcmp(ctx->last_sent_packet_data, data_content) == 0) {    // 변경된 내용이 없으면 안보냄
        // logger_log(LOG_LEVEL_DEBUG, "skipping msg %s[%s]-> msg: %s = %s", ctx->name, ctx->ip, ctx->last_sent_packet_data, data_content);
        return;
    }
    uint8_t packet[1024];
    int len = make_m30_packet(packet, 0x84, data_content); // 0x84: INSERT
    if (SendBuf(ctx->handle, (char*)packet, len)) {
        logger_log(LOG_LEVEL_DEBUG, "Sent to %s: %s", ctx->name, data_content);
        strncpy(ctx->last_sent_packet_data, data_content, sizeof(ctx->last_sent_packet_data));
    } else {
        logger_log(LOG_LEVEL_WARN, "Send Fail to %s", ctx->name);
        // 전송 실패 시 연결 끊김 처리
        CommClose(ctx->handle);
        ctx->handle = -1;
        ctx->connected = false;
    }
}

void send_m30_brightness(M30_CONTEXT *ctx, int level) {
    if (!ctx->connected || ctx->handle == -1) return;
    if (ctx->last_brightness == level) return; // 변경 없으면 생략
    const uint8_t brt_packets[10][18] = {   // 밝기 변경 패킷 (얜 프로토콜 문서랑 다름)
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x31, 0x00, 0x12, 0x03}, // 밝기 1
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x32, 0x00, 0x13, 0x03}, // 밝기 2
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x33, 0x00, 0x14, 0x03}, // 밝기 3
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x34, 0x00, 0x15, 0x03}, // 밝기 4
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x35, 0x00, 0x16, 0x03}, // 밝기 5
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x36, 0x00, 0x17, 0x03}, // 밝기 6
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x37, 0x00, 0x18, 0x03}, // 밝기 7
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x38, 0x00, 0x19, 0x03}, // 밝기 8
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x30, 0x00, 0x39, 0x00, 0x1A, 0x03}, // 밝기 9
        {0x02, 0x89, 0x0C, 0x00, 0x42, 0x00, 0x52, 0x00, 0x49, 0x00, 0x3D, 0x00, 0x31, 0x00, 0x30, 0x00, 0x1B, 0x03}  // 밝기 10
    };
    const size_t packet_len = 18;
    uint8_t* brt_packet_to_send = brt_packets[level - 1];
    if (SendBuf(ctx->handle, (char*)brt_packet_to_send, packet_len)) {
        logger_log(LOG_LEVEL_DEBUG, "Brightness Set %s: %d", ctx->name, level);
        ctx->last_brightness = level;
    }
}

// ============================ 표출 로직 (애니메이션) ============================
// void process_group_logic(int grp_idx, int msg_id, int speed, int pet_gap) {  // 특정 그룹의 메시지 인덱스를 읽고 적절한 표출 명령 전송
//     if (g_group_dev_cnt[grp_idx] == 0) return;   // 그룹이 없으면 바로 리턴

//     for (int i = 0; i < g_group_dev_cnt[grp_idx]; i++) { // 해당 그룹의 개별 장치마다 순회
//         M30_CONTEXT *ctx = g_group_map[grp_idx][i];
//         if (!ctx) continue;

//         char command_str[256];
//         bool turn_on = false;

//         if (msg_id == 0) {  // 0번 이미지 표출
//             turn_on = false;
//         }

//         if (msg_id == 1) { // 단독주행
//             // speed 값을 틱 단위로 (50ms 배수)
//             int spd_val = (speed <= 0) ? 1 : speed;
//             int delay_ticks = 11 - spd_val;
//             if (delay_ticks < 1) delay_ticks = 1;   // 가장 빠르면 50ms 주기 (process_group_logic 1회 실행 시마다)
//             if (delay_ticks > 10) delay_ticks = 10; // 느리면 500ms 주기

//             if ((g_group_anim[grp_idx].wave_idx == 0) && (g_group_anim[grp_idx].wave_count == 0)) {  // 첫 웨이브 시작이거나, 돌아오는 웨이브일 때
//                 g_group_anim[grp_idx].wave_phase = !g_group_anim[grp_idx].wave_phase;   // 끄고 켜는 플래그 반전
//             }
//             if (i == g_group_dev_cnt[grp_idx]-1) { // 이 그룹을 다 돌았으면 wave_count 올리기
//                 g_group_anim[grp_idx].wave_count = (g_group_anim[grp_idx].wave_count+1) % delay_ticks;
//             }
//             if (g_group_anim[grp_idx].wave_count == 0) {    // 틱이 차면 보내야할 장치에 표출 설정
//                 if (i == g_group_anim[grp_idx].wave_idx) {
//                     turn_on = g_group_anim[grp_idx].wave_phase; // 표출 설정
//                     g_group_anim[grp_idx].wave_idx = (g_group_anim[grp_idx].wave_idx+1) % g_group_dev_cnt[grp_idx]; // 다음 표출할 장치 인덱스 설정
//                 } else { continue; }    // 표출할 장치가 아니면 스킵
//             }
//         } else {    // 단독주행이 아니면 초기화
//             g_group_anim[grp_idx].wave_count = 0;
//             g_group_anim[grp_idx].wave_idx = 0;
//             g_group_anim[grp_idx].wave_phase = false;
//         }

//         if (msg_id == 2) { // 상충경고
//             int pet_ticks = (pet_gap <= 0) ? 1 : pet_gap;
//             if (pet_ticks < 1) pet_ticks = 1;   // 가장 빠르면 50ms 주기
//             if (pet_ticks > 10) pet_ticks = 10; // 느리면 500ms 주기

//             if (g_group_anim[grp_idx].blink_count == 0) {  // 첫 웨이브 시작이거나, 돌아오는 웨이브일 때
//                 g_group_anim[grp_idx].blink_phase = !g_group_anim[grp_idx].blink_phase; // 표출 위상 반전
//             }
//             if (i == g_group_dev_cnt[grp_idx]-1) { // 이 그룹을 다 돌았으면 wave_count 올리기
//                 g_group_anim[grp_idx].blink_count = (g_group_anim[grp_idx].blink_count+1) % pet_ticks;
//             }
//             if (g_group_anim[grp_idx].blink_count == 0) {    // 틱이 차면 전체 장치에 표출 설정
//                 turn_on = g_group_anim[grp_idx].blink_phase;
//             }
//         } else {    // 상충이 아니면 초기화
//             g_group_anim[grp_idx].blink_count = 0;
//             g_group_anim[grp_idx].blink_phase = false;
//         }

//         // 패킷 전송
//         if (turn_on) {
//             if (msg_id == 1) {
//                 snprintf(command_str, sizeof(command_str), "RST=1 USM=001");
//             } else {
//                 snprintf(command_str, sizeof(command_str), "RST=1 USM=002");
//             }
//         } else {
//             snprintf(command_str, sizeof(command_str), "RST=1 USM=000");
//         }
//         send_m30_data_packet(ctx, command_str);
//     }
// }


void process_group_logic(int grp_idx, int msg_id, int speed, int pet_gap) { // todo. 제대로 동작하는지 테스트
    if (g_group_dev_cnt[grp_idx] == 0) return; // 그룹이 없으면 리턴

    if (msg_id == 1) {
        // todo. 주기 계산 함수는 현장 테스트로 조절하기
        int spd_val = (speed <= 0) ? 1 : speed;
        int delay_ticks = 11 - spd_val;
        if (delay_ticks < 1) delay_ticks = 1;

        g_group_anim[grp_idx].wave_count++;
        if (g_group_anim[grp_idx].wave_count >= delay_ticks) {  // 틱이 돌면 위상을 조절해서 딜레이 표현
            g_group_anim[grp_idx].wave_count = 0;
            g_group_anim[grp_idx].wave_idx++;
            if (g_group_anim[grp_idx].wave_idx >= g_group_dev_cnt[grp_idx]) {
                g_group_anim[grp_idx].wave_idx = 0;
                g_group_anim[grp_idx].wave_phase = !g_group_anim[grp_idx].wave_phase;
            }
        }
    } else {
        // 메시지가 1번이 아니면 상태 초기화 (다시 1번이 들어오면 처음부터 켜지도록)
        g_group_anim[grp_idx].wave_count = 0;
        g_group_anim[grp_idx].wave_idx = 0;
        g_group_anim[grp_idx].wave_phase = true; // 켜는 단계부터
    }
    if (msg_id == 2) {
        // todo. 주기 계산 함수는 현장 테스트로 조절하기
        int pet_ticks = (pet_gap <= 0) ? 1 : pet_gap;
        if (pet_ticks < 1) pet_ticks = 1;
        if (pet_ticks > 10) pet_ticks = 10;

        g_group_anim[grp_idx].blink_count++;
        if (g_group_anim[grp_idx].blink_count >= pet_ticks) {
            g_group_anim[grp_idx].blink_count = 0;
            g_group_anim[grp_idx].blink_phase = !g_group_anim[grp_idx].blink_phase;
        }
    } else {
        // 메시지가 2번이 아니면 초기화
        g_group_anim[grp_idx].blink_count = 0;
        g_group_anim[grp_idx].blink_phase = true;
    }

    for (int i = 0; i < g_group_dev_cnt[grp_idx]; i++) {    // 각 장치별 순회하며 메시지 전송
        M30_CONTEXT *ctx = g_group_map[grp_idx][i];
        if (!ctx) continue;

        char command_str[256];
        bool turn_on = false;

        if (msg_id == 0) {
            turn_on = false; // 전체 소등
        }
        else if (msg_id == 1) { // 단독주행: 순차 점등/소등
            if (g_group_anim[grp_idx].wave_phase == true) { // 켜는 단계면
                if (i <= g_group_anim[grp_idx].wave_idx) turn_on = true;    // 켜야할 인덱스까지 on (중복 전송 방지 있으니까 상관없음)
                else turn_on = false;
            } else {    // 끄는 단계면
                if (i > g_group_anim[grp_idx].wave_idx) turn_on = true; // 꺼야할 인덱스까지 off
                else turn_on = false;
            }
        }
        else if (msg_id == 2) { // 상충 경고: 전체 점멸
            turn_on = g_group_anim[grp_idx].blink_phase;
        }

        if (turn_on) {  // 최종 전송
            if (msg_id == 1) {
                snprintf(command_str, sizeof(command_str), "RST=1 USM=001");
            } else {
                snprintf(command_str, sizeof(command_str), "RST=1 USM=002");
            }
        } else {
            snprintf(command_str, sizeof(command_str), "RST=1 USM=000");
        }
        send_m30_data_packet(ctx, command_str);
    }
}
//=====================================================================================

void process_all_groups() {
    // SHM에서 명령 읽어오기
    // vms_command_ptr->n_in_msg[] : [0]=msg_id, [1]=speed, [2]=pet_gap

    if (connection_status_ptr->ig_server_conn == false) {   // 서버랑 연결 끊기면 전체 off
        for (int g = 0; g < TOTAL_GROUPS; g++) {
            process_group_logic(g, 0, 0, 0);
        }
        return; 
    }

    #define PROC_GRP(GRP_ENUM, CMD_ARR) \
        process_group_logic(GRP_ENUM, CMD_ARR[0], CMD_ARR[1], CMD_ARR[2])

    PROC_GRP(GRP_N_IN,   vms_command_ptr->n_in_msg);    // 각 그룹에 표출 설정
    PROC_GRP(GRP_N_LOAD, vms_command_ptr->n_load_msg);
    PROC_GRP(GRP_N_OUT,  vms_command_ptr->n_out_msg);

    PROC_GRP(GRP_E_IN,   vms_command_ptr->e_in_msg);
    PROC_GRP(GRP_E_LOAD, vms_command_ptr->e_load_msg);
    PROC_GRP(GRP_E_OUT,  vms_command_ptr->e_out_msg);

    PROC_GRP(GRP_S_IN,   vms_command_ptr->s_in_msg);
    PROC_GRP(GRP_S_LOAD, vms_command_ptr->s_load_msg);
    PROC_GRP(GRP_S_OUT,  vms_command_ptr->s_out_msg);

    PROC_GRP(GRP_W_IN,   vms_command_ptr->w_in_msg);
    PROC_GRP(GRP_W_LOAD, vms_command_ptr->w_load_msg);
    PROC_GRP(GRP_W_OUT,  vms_command_ptr->w_out_msg);
}

// ============================ 밝기 제어 ============================
void process_brightness() {
    static time_t last_brt_time = 0;
    if (nowtime - last_brt_time < 1) return; // 1초 주기
    last_brt_time = nowtime;

    float lux = vms_command_ptr->brightness;
    int target_level = 1;

    // 조도(Lux)에 따른 밝기 레벨 매핑  // todo. 실제 현장에 설치하고 조정해야함
    if (lux < 0) target_level = 5; // 센서 에러 시 중간값
    else if (lux < 100) target_level = 1; // 야간
    else if (lux < 500) target_level = 3;
    else if (lux < 2000) target_level = 6;
    else target_level = 10; // 주간

    for(int i=0; i<g_m30_count; i++) {
        send_m30_brightness(&g_m30_devs[i], target_level);
    }
}

// ============================ 수신 루프 ============================
void packet_loop() {
    int nRead = 0;
    BOOL res;

    for (int i = 0; i < g_m30_count; i++) {
        M30_CONTEXT *ctx = &g_m30_devs[i];
        if (!ctx->connected || ctx->handle == -1) continue; // 연결 안 돼 있으면 안읽음

        size_t space = RECV_BUF_SIZE - ctx->buf_len;
        if (space == 0) {
            ctx->buf_len = 0; 
            space = RECV_BUF_SIZE;
        }

        // Non-blocking 수신
        res = RecvBuf(ctx->handle, (char*)(ctx->recv_buf + ctx->buf_len), space, &nRead, 0);

        if (res == TRUE && nRead > 0) {
            ctx->buf_len += nRead;
            // 여기서 ACK 확인 등 처리 가능 (현재는 연결 유지 확인용으로만 사용)
            if (ctx->buf_len > 0) ctx->buf_len = 0; // 버퍼 비우기
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

// ============================ 메시지 큐 처리 스레드 ============================
void message_analy(BYTE *_pData) {
	if(_pData[0] != (0x80 | M30_MANAGER_Q_PID)) return;
    // 필요 시 외부 제어 명령 처리
}

void *do_thread(void *data) {   // 연결 관리를 여기서 함. // todo. 현재 블로킹 방식이라 하나씩 확인할 때 엄청 오래 걸림. 바꿀 수 있는지 확인 요
	// BYTE recv_msg[1024];
	// int recv_size = 0;
	while (1) {
		usleep(100000); // 100ms
        for (int i = 0; i < g_m30_count; i++) {
            M30_CONTEXT *ctx = &g_m30_devs[i];
            if (!ctx->connected) {
                logger_log(LOG_LEVEL_INFO, "%s[%s] 연결 유실, 재시도", ctx->name, ctx->ip);
                // connected가 false일 때는 main에서 송신이나 수신이 안 될 때 뿐이고, 그 땐 이미 연결 해제 + 핸들도 -1로 초기화한 이후니까 문제없을듯
                THANDLEINDEX new_handle = CommInit(CONNECT, ctx->ip, ctx->port, true);
                if (new_handle != -1) {
                    logger_log(LOG_LEVEL_INFO, "M30 Connected: %s[%s]", ctx->name, ctx->ip);
                    ctx->buf_len = 0;
                    memset(ctx->last_sent_packet_data, 0, sizeof(ctx->last_sent_packet_data));
                    ctx->last_brightness = -1;
                    ctx->handle = new_handle;
                    ctx->connected = true;
                } else {
                    logger_log(LOG_LEVEL_ERROR, "M30 Connect Fail: %s[%s]", ctx->name, ctx->ip);
                }
            }
        }
        // 메시지 큐 읽기 (일단 쓸 일 없음)
        // recv_size = 0;
        // bool has_msg = false;
        // while (system_set_ptr->msg_M30_Q.head != system_set_ptr->msg_M30_Q.tail) {
        //     recv_msg[recv_size++] = system_set_ptr->msg_M30_Q.buffer[system_set_ptr->msg_M30_Q.tail];
        //     system_set_ptr->msg_M30_Q.tail++;
        //     if (system_set_ptr->msg_M30_Q.tail >= BUF_SIZE)
        //         system_set_ptr->msg_M30_Q.tail = 0;
        //     has_msg = true;
        // }
        // if (has_msg) message_analy(recv_msg);
    }
}

// ============================ MAIN ============================
int main() {
    if (logger_init("Logs/M30_Manager_Log", 100) != 0) {
        printf("Logger init failed\n");
    }

    if (shm_all_open() == false) {
        logger_log(LOG_LEVEL_ERROR,"M30_Mgr] shm_init failed");
        exit(-1);
    }
    if (msg_all_open() == false) {
        logger_log(LOG_LEVEL_ERROR,"M30_Mgr] msg_init failed");
    }

    signal(SIGINT, handle_sigint);

    proc_shm_ptr->pid[M30_MANAGER_PID] = getpid();
    proc_shm_ptr->status[M30_MANAGER_PID] = 'S';

    // 장치 목록 초기화
    init_device_list();
    
    pthread_t p_thread;
    if (pthread_create(&p_thread, NULL, do_thread, NULL) != 0) {
        logger_log(LOG_LEVEL_ERROR, "Thread creation failed");
        exit(EXIT_FAILURE);
    }

    logger_log(LOG_LEVEL_INFO, "M30 Manager Start. Devices: %d", g_m30_count);

    while (1) {
        usleep(50000); // 50ms (애니메이션 부드러움을 위해 단축)
        nowtime = time(NULL);
        // manage_connections(); // 연결 관리
        packet_loop();        // 수신 처리
        process_all_groups(); // 표출 로직
        process_brightness(); // 밝기 제어
    }
    return 0;
}