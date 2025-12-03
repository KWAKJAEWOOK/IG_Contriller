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
#include <string.h>

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

//============================== 소소한 헬퍼함수 =============================
void safe_strcpy(char* dest, cJSON* item, size_t max_len) {	// json 객체에서 문자열 복사할때 버퍼 오버플로우 방지 및 NULL 체크
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        strncpy(dest, item->valuestring, max_len - 1);
        dest[max_len - 1] = '\0';
    } else {
        dest[0] = '\0';
    }
}
//===========================================================================

//======================== scenaroi.CSV 파일 관련 =======================
typedef struct {	// scenario.CSV 읽어서 저장해두는 구조체
    int idx;
    int ho_entry;  // HO 교차로 진입 방향 코드
    int ho_egress; // HO 교차로 진출 방향 코드
    int ro_entry;  // RO 교차로 진입 방향 코드

    // 각 그룹별 표출 메시지 인덱스
    int n_in, n_load, n_out;
    int e_in, e_load, e_out;
    int s_in, s_load, s_out;
    int w_in, w_load, w_out;
} SCENARIO_ROW;

#define MAX_SCENARIO_ROWS 81	// csv 파일에 들어간 idx 최댓값
SCENARIO_ROW g_scenario_table[MAX_SCENARIO_ROWS];
int g_scenario_count = 0;

void load_scenario_csv() {	// scenario.CSV 파일 읽어서 구조체에 저장해두기
    FILE *fp = fopen("init/scenario.CSV", "r");
    if (!fp) {
        logger_log(LOG_LEVEL_ERROR, "Failed to open init/scenario.CSV");
        return;
    }

    char line[1024];
    int row = 0;

    // 첫 줄(헤더) 건너뛰기
    fgets(line, sizeof(line), fp);

    while (fgets(line, sizeof(line), fp) && row < MAX_SCENARIO_ROWS) {
        SCENARIO_ROW *r = &g_scenario_table[row];
        int count = sscanf(line, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
            &r->idx, &r->ho_entry, &r->ho_egress, &r->ro_entry,
            &r->n_in, &r->n_load, &r->n_out,
            &r->e_in, &r->e_load, &r->e_out,
            &r->s_in, &r->s_load, &r->s_out,
            &r->w_in, &r->w_load, &r->w_out);
        if (count >= 16) { // 한줄에 데이터가 빠져있으면 스킵
            row++;
        }
    }
    g_scenario_count = row;
    fclose(fp);
    logger_log(LOG_LEVEL_INFO, "Scenario Loaded. Total rows: %d", g_scenario_count);
}

//==================================== VMS 제어용 공유메모리 업데이트 관련 =====================================

void estimation_direction_code() {	// Waypoint의 GPS 값을 가지고 객체의 진입/진출 방향을 추정하는 코드 (CVIBDirCode 추가되면 사용 축소 가능)
	// todo. 객체의 WaypointList 데이터를 활용해서, system_set_ptr 내부에 저장된 CVIBDirCode로 변환하기
	// todo. 변환한 CVIBDirCode로 HO_Egress_DC, RO_Entry_DC를 만들고 CSV 파일과 대조해서 M30 그룹별 표출 메시지 인덱스 결정
}

void update_vms_group(int *msg_group, int new_msg_id, int speed, int petgap) {	// 각 그룹 별로 최종 메시지 인덱스랑 speed, petgap 값 업데이트. 표출 우선도 관련 로직 들어가있음
	// msg_group[0]: MsgID, [1]: Speed, [2]: PETGap(Threshold - PET)
	if (new_msg_id > msg_group[0]) {	// new_msg_id가 기존보다 클 때
		if (new_msg_id == 1) {	// 업데이트할 메시지 인덱스가 HO 단독 주행일 때
			msg_group[0] = new_msg_id;
			msg_group[1] = speed;
			// petgap은 어짜피 표출때 안쓰니까 업데이트 안함
		} else if (new_msg_id == 2) {	// 상충 경고일 때는 걍 다 갈아치우기
			msg_group[0] = new_msg_id;
			msg_group[1] = speed;
			msg_group[2] = petgap;
		}
	} else if (new_msg_id == msg_group[0]) {	// 같은 메시지 표출이라면
		if (new_msg_id == 1) {	// HO 단독주행일 때
			msg_group[1] = (speed>=msg_group[1])?speed:msg_group[1];	// 더 빠른 객체껄로 업데이트
			// petgap은 어짜피 표출때 안쓰니까 업데이트 안함
		} else if (new_msg_id == 2) {	// 상충 경고면
			if (petgap < msg_group[2]) {	// 더 위험할때만 업데이트
				msg_group[1] = speed;
				msg_group[2] = petgap;
			}
		}
	}

    
    if (msg_group[0] < new_msg_id || petgap > msg_group[2]) {	// 기존 MsgID 인덱스가 작거나(표출 우선도가 낮음), 더 위험한 상황(Gap이 더 큼)이면 업데이트
        msg_group[0] = new_msg_id;
        msg_group[1] = speed;
        msg_group[2] = petgap;
    }
}

void calc_vms_command() {	// JSON 파싱 끝나고 VMS 제어용 정보 생성하기
	// 기존 메시지값들 초기화 (객체 없으면 0번 인덱스의 메시지 뿌려야됨)
	memset(vms_command_ptr->n_in_msg, 0, sizeof(vms_command_ptr->n_in_msg));
    memset(vms_command_ptr->n_load_msg, 0, sizeof(vms_command_ptr->n_load_msg));
    memset(vms_command_ptr->n_out_msg, 0, sizeof(vms_command_ptr->n_out_msg));
    memset(vms_command_ptr->e_in_msg, 0, sizeof(vms_command_ptr->e_in_msg));
    memset(vms_command_ptr->e_load_msg, 0, sizeof(vms_command_ptr->e_load_msg));
    memset(vms_command_ptr->e_out_msg, 0, sizeof(vms_command_ptr->e_out_msg));
    memset(vms_command_ptr->s_in_msg, 0, sizeof(vms_command_ptr->s_in_msg));
    memset(vms_command_ptr->s_load_msg, 0, sizeof(vms_command_ptr->s_load_msg));
    memset(vms_command_ptr->s_out_msg, 0, sizeof(vms_command_ptr->s_out_msg));
    memset(vms_command_ptr->w_in_msg, 0, sizeof(vms_command_ptr->w_in_msg));
    memset(vms_command_ptr->w_load_msg, 0, sizeof(vms_command_ptr->w_load_msg));
    memset(vms_command_ptr->w_out_msg, 0, sizeof(vms_command_ptr->w_out_msg));

	// 디버깅용 MsgCount랑 Timestamp 복사
	strcpy(vms_command_ptr->Timestamp, message_data_ptr->Timestamp);
    vms_command_ptr->MsgCount = message_data_ptr->MsgCount;

	for (int i = 0; i < message_data_ptr->Num_Of_ApproachTrafficInfo; i++) {	// 공유메모리의 message_data_ptr->ApproachTrafficInfo 순회하면서 시나리오랑 매칭
		int PETGap_i = (int)(message_data_ptr->ApproachTrafficInfo[i].PET_Threshold - message_data_ptr->ApproachTrafficInfo[i].PET);	// RO가 없으면 걍 큰값으로 남겠지머
		int ho_entry_i = message_data_ptr->ApproachTrafficInfo[i].HostObject.CVIBDirCode;

		int ho_egress_i = 0;
		if (message_data_ptr->ApproachTrafficInfo[i].HostObject.Num_Of_HO_WayPoint > 0) {	// HO의 Waypoint가 있으면
			// todo. estimation_direction_code()로 ho_egress_i 채우기
		}	// Waypoint 없으면 ho_egress_i는 걍 0으로 두기

		int ro_entry_i = 0;
		if (message_data_ptr->ApproachTrafficInfo[i].PET != -1) {	// RO가 존재하면
			// todo. estimation_direction_code()로 ro_entry_i 채우기
		} // RO가 없으면 ro_entry_i는 걍 0으로 두기

		int speed_i = 0;
		if (message_data_ptr->ApproachTrafficInfo[i].HostObject.Num_Of_HO_WayPoint > 0) {	// HO Waypoint가 있으면, 첫번째 포인트의 속도값 긁어오기
            speed_i = (int)(message_data_ptr->ApproachTrafficInfo[i].HostObject.WayPoint[0].speed);
        }	// 없으면 0으로 두기

		// todo. 시나리오랑 매칭
		for (int j = 0; j < g_scenario_count; j++) {
            SCENARIO_ROW *row_j = &g_scenario_table[j];
            if (row_j->ho_entry == ho_entry_i && 
                row_j->ho_egress == ho_egress_i && 
                row_j->ro_entry == ro_entry_i) {
                
                // 매칭된 시나리오의 메시지 ID를 각 그룹에 업데이트
                update_vms_group(vms_command_ptr->n_in_msg, row_j->n_in, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->n_load_msg, row_j->n_load, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->n_out_msg, row_j->n_out, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->e_in_msg, row_j->e_in, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->e_load_msg, row_j->e_load, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->e_out_msg, row_j->e_out, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->s_in_msg, row_j->s_in, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->s_load_msg, row_j->s_load, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->s_out_msg, row_j->s_out, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->w_in_msg, row_j->w_in, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->w_load_msg, row_j->w_load, speed_i, PETGap_i);
                update_vms_group(vms_command_ptr->w_out_msg, row_j->w_out, speed_i, PETGap_i);
                break;
            }
        }
	}
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
    const cJSON* json_MsgCount = cJSON_GetObjectItemCaseSensitive(json_root, "MsgCount");
	if (cJSON_IsNumber(json_MsgCount)) { message_data_ptr->MsgCount; }
	const cJSON* json_Timestamp = cJSON_GetObjectItemCaseSensitive(json_root, "Timestamp");
    if (cJSON_IsString(json_Timestamp)) { strcpy(message_data_ptr->Timestamp, json_Timestamp->valuestring); }

    cJSON* json_ApproachTrafficInfoList = cJSON_GetObjectItem(json_root, "ApproachTrafficInfoList");	// ApproachTrafficInfoList 배열 파싱
    if (cJSON_IsArray(json_ApproachTrafficInfoList)) {
        cJSON* json_ApproachTrafficInfo = NULL;
		int traffic_info_index = 0;
        cJSON_ArrayForEach(json_ApproachTrafficInfo, json_ApproachTrafficInfoList) {	// 각 ApproachTrafficInfo 순회하기
            cJSON* json_conflictPos = cJSON_GetObjectItem(json_ApproachTrafficInfo, "ConflictPos");	// ConflictPos 객체 파싱
            if (json_conflictPos && cJSON_IsObject(json_conflictPos)) {	// 상충경고가 있으면
                const cJSON* json_Conflict_lat = cJSON_GetObjectItemCaseSensitive(json_conflictPos, "lat");
				const cJSON* json_Conflict_lon = cJSON_GetObjectItemCaseSensitive(json_conflictPos, "lon");
				const cJSON* json_PET = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "PET");
				if (cJSON_IsNumber(json_Conflict_lat) && cJSON_IsNumber(json_Conflict_lon) && cJSON_IsNumber(json_PET)) {
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].ConflictPos.lat = json_Conflict_lat->valuedouble;
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].ConflictPos.lon = json_Conflict_lon->valuedouble;
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].PET = json_PET->valuedouble;
				}
            } else {	// 상충경고 없으면 다 -1로 채우자
				message_data_ptr->ApproachTrafficInfo[traffic_info_index].ConflictPos.lat = -1;
				message_data_ptr->ApproachTrafficInfo[traffic_info_index].ConflictPos.lon = -1;
				message_data_ptr->ApproachTrafficInfo[traffic_info_index].PET = -1;
			}
			const cJSON* json_PET_Threshold = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "PET_Threshold");
			if (cJSON_IsNumber(json_PET_Threshold)) {
				message_data_ptr->ApproachTrafficInfo[traffic_info_index].PET_Threshold = json_PET_Threshold->valuedouble;
			}

			const cJSON* json_HostObject = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "HostObject");	// HO 파싱
			if (cJSON_IsObject(json_HostObject)) {
				const cJSON* json_HO_ObjectType = cJSON_GetObjectItemCaseSensitive(json_HostObject, "ObjectType");
				const cJSON* json_HO_ObjectID = cJSON_GetObjectItemCaseSensitive(json_HostObject, "ObjectID");
				if (cJSON_IsString(json_HO_ObjectType) && cJSON_IsString(json_HO_ObjectID)) {
					safe_strcpy(&message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectType, json_HO_ObjectType->valuestring
						, sizeof(message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectType));
					safe_strcpy(&message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectID, json_HO_ObjectID->valuestring
						, sizeof(message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectID));
				}
				/*
				todo. 위치 이동 후 주석 제거, 아래꺼 삭제
				const cJSON* json_HostObj_CVIBDirCode = cJSON_GetObjectItemCaseSensitive(json_HostObject, "CVIBDirCode");
				if (cJSON_IsNumber(json_HostObj_CVIBDirCode)) {
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.CVIBDirCode = json_HostObj_CVIBDirCode->valueint;
				}
				*/
				const cJSON* json_HostObj_CVIBDirCode = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "CVIBDirCode");
				if (cJSON_IsNumber(json_HostObj_CVIBDirCode)) {
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.CVIBDirCode = json_HostObj_CVIBDirCode->valueint;
				}

				const cJSON* json_HO_WayPointList = cJSON_GetObjectItemCaseSensitive(json_HostObject, "WayPointList");
				if (cJSON_IsArray(json_HO_WayPointList)) {
					cJSON* json_WayPoint = NULL;
					int wayPoint_index = 0;
					cJSON_ArrayForEach(json_WayPoint, json_HO_WayPointList) {	// 각 WayPoint 순회하기
						if (json_WayPoint){	// "WayPointList":[]처럼 빈 리스트일 수 있으니까
							const cJSON* json_WayPoint_lat = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lat");
							const cJSON* json_WayPoint_lon = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lon");
							const cJSON* json_WayPoint_speed = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "speed");
							if (cJSON_IsNumber(json_WayPoint_lat) && cJSON_IsNumber(json_WayPoint_lon) && cJSON_IsNumber(json_WayPoint_speed)) {
								message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].lat = json_WayPoint_lat->valuedouble;
								message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].lon = json_WayPoint_lon->valuedouble;
								message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].speed = json_WayPoint_speed->valuedouble;
							}
						}
						wayPoint_index++;
					}
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.Num_Of_HO_WayPoint = wayPoint_index;
				}
			}
			const cJSON* json_RemoteObject = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "RemoteObject");	// RO 파싱
			if (json_RemoteObject && cJSON_IsObject(json_RemoteObject)) {	// 얜 없을수도있음
				const cJSON* json_RO_ObjectType = cJSON_GetObjectItemCaseSensitive(json_HostObject, "ObjectType");
				const cJSON* json_RO_ObjectID = cJSON_GetObjectItemCaseSensitive(json_HostObject, "ObjectID");
				if (cJSON_IsString(json_RO_ObjectType) && cJSON_IsString(json_RO_ObjectID)) {
					safe_strcpy(&message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectType, json_RO_ObjectType->valuestring
						, sizeof(message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectType));
					safe_strcpy(&message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectID, json_RO_ObjectID->valuestring
						, sizeof(message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectID));
				}
				/*
				todo. 웨이즈원이 추가해주면 주석 제거하기
				const cJSON* json_RemoteObj_CVIBDirCode = cJSON_GetObjectItemCaseSensitive(RemoteObject, "CVIBDirCode");
				if (cJSON_IsNumber(json_RemoteObj_CVIBDirCode)) {
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.CVIBDirCode = json_RemoteObj_CVIBDirCode->valueint;
				}
				*/
				const cJSON* json_RO_WayPointList = cJSON_GetObjectItemCaseSensitive(json_RemoteObject, "WayPointList");
				if (cJSON_IsArray(json_RO_WayPointList)) {
					cJSON* json_WayPoint = NULL;
					int wayPoint_index = 0;
					cJSON_ArrayForEach(json_WayPoint, json_RO_WayPointList) {	// 각 WayPoint 순회하기
						if (json_WayPoint){	// "WayPointList":[]처럼 빈 리스트일 수 있으니까
							const cJSON* json_WayPoint_lat = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lat");
							const cJSON* json_WayPoint_lon = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lon");
							const cJSON* json_WayPoint_speed = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "speed");
							if (cJSON_IsNumber(json_WayPoint_lat) && cJSON_IsNumber(json_WayPoint_lon) && cJSON_IsNumber(json_WayPoint_speed)) {
								message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].lat = json_WayPoint_lat->valuedouble;
								message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].lon = json_WayPoint_lon->valuedouble;
								message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].speed = json_WayPoint_speed->valuedouble;
							}
						}
						wayPoint_index++;
					}
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.Num_Of_RO_WayPoint = wayPoint_index;
				}
			}
            traffic_info_index++;
        }
		message_data_ptr->Num_Of_ApproachTrafficInfo = traffic_info_index;
    }
	calc_vms_command();	// 파싱 후 VMS 제어용 정보 공유메모리에 업데이트하기
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
// ============================================================================
void handle_sigint(int sig) {	// Ctrl+C 종료 처리 핸들러
    printf("\nIG_Server_Manager Terminating... (Signal: %d)\n", sig);
    if (shm_close() != 0) {
        printf("shm_close() failed\n");
    }
    // 필요 시 소켓 close 추가
    exit(0);
}
// ============================================================================
int main()
{
	int i;
	bool bReturn;

	if (logger_init("Logs/IG_Server_Manager_Log", 100) != 0) {	// 로거 테스트용 100mb
		logger_log(LOG_LEVEL_ERROR, "Logger init failed");
        exit(EXIT_FAILURE);
    }
	// todo. 디버깅을 위해 IG-Server에서 수신한 rawdata, message_data_ptr 데이터, vms_command_ptr 데이터를 각각 다른 디렉토리에 로깅하고싶은데

	logger_log(LOG_LEVEL_INFO, "IG-Server Manager Start.");

	if (shm_all_open() == false)
	{
		logger_log(LOG_LEVEL_ERROR, "IG_Server] shm_init() failed");
		exit(-1);
	}

	if (msg_all_open() == false)
	{
		logger_log(LOG_LEVEL_ERROR, "IG_Server] msg_all_open() failed");
		exit(-1);
	}

	load_scenario_csv();	// scenario.CSV 읽어서 저장해두기

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
