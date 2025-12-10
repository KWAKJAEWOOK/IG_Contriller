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
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"
#include "global/logger.h"
#include "global/cJSON.h"

time_t nowtime;
time_t last_keep_alive_time;
THANDLEINDEX HandleIndex = -1;

//============================== 소소한 헬퍼함수 =============================
// void safe_strcpy(char* dest, cJSON* item, size_t max_len) {	// json 객체에서 문자열 복사할때 버퍼 오버플로우 방지 및 NULL 체크
//     if (cJSON_IsString(item) && (item->valuestring != NULL)) {
//         strncpy(dest, item->valuestring, max_len - 1);
//         dest[max_len - 1] = '\0';
//     } else {
//         dest[0] = '\0';
//     }
// }

int convert_dircode_to_count(int dircode) {	// CVIBDircode를 scenario.csv에 등록된 방향 인덱스로 변환하는 함수
	switch (dircode)
	{
		case 10:	// 북
			return 1;
		case 50:	// 북동
			return 1;
		case 20:	// 동
			return 2;
		case 60:	// 남동
			return 2;
		case 30:
			return 3;
		case 70:
			return 3;
		case 40:
			return 4;
		case 80:
			return 4;
		case 0:
			return 0;
	}
}

//========================== 데이터 로깅을 위한 함수 =============================
#define SPECIFIC_LOG_LIMIT_MB 500	// 각 로그 폴더 최대 용량

typedef enum {
    LOG_TYPE_RAW,   // IG-Server에서 수신한 Raw Data (JSON)
    LOG_TYPE_SHM,   // Raw Data 1차 가공해서 줄인 간소화 데이터
    LOG_TYPE_VMS    // 간소화시킨 데이터로 만든, 최종 표출 결과 로깅
} LOG_DATA_TYPE;

long long get_specific_dir_size(const char* path) {	// 디렉토리 크기 계산
    long long total_size = 0;
    DIR *d = opendir(path);
    if (!d) return 0;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
        
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, dir->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            total_size += st.st_size;
        }
    }
    closedir(d);
    return total_size;
}
void remove_oldest_specific_log(const char* dir_path) {	// 가장 오래된 로그 파일 삭제
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *dir;
    char oldest_file[512] = "";
    time_t oldest_time = 0;

    while ((dir = readdir(d)) != NULL) {
        if (strstr(dir->d_name, ".log")) { // log 파일만
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir->d_name);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                if (oldest_time == 0 || st.st_mtime < oldest_time) {
                    oldest_time = st.st_mtime;
                    strcpy(oldest_file, full_path);
                }
            }
        }
    }
    closedir(d);

    if (strlen(oldest_file) > 0) {
        remove(oldest_file);
        // logger_log(LOG_LEVEL_INFO, "Deleted old specific log: %s", oldest_file); 
    }
}

void Log_data(LOG_DATA_TYPE type, const char* fmt, ...) {	// 디버깅을 위한 데이터 로깅용 함수
	char dir_path[128];
    char file_path[256];
    char date_str[32];
	switch(type) {	// 타입별 디렉토리 경로 설정
        case LOG_TYPE_RAW: strcpy(dir_path, "Logs/IG_Server_Manager_Log/RawData"); break;
        case LOG_TYPE_SHM: strcpy(dir_path, "Logs/IG_Server_Manager_Log/ShmData"); break;
        case LOG_TYPE_VMS: strcpy(dir_path, "Logs/IG_Server_Manager_Log/VmsCmd"); break;
        default: return;
    }

	struct stat st = {0};
    if (stat(dir_path, &st) == -1) {
        mkdir(dir_path, 0777);	// 디렉토리 없으면 만들어
    }

	long long limit_bytes = (long long)SPECIFIC_LOG_LIMIT_MB * 1024 * 1024;	// 디렉토리에 용량이 넘치면 삭제
    if (get_specific_dir_size(dir_path) > limit_bytes) {
        remove_oldest_specific_log(dir_path);
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", (t->tm_year + 1900), (t->tm_mon + 1), (t->tm_mday));
    snprintf(file_path, sizeof(file_path), "%s/%s.log", dir_path, date_str);

    FILE *fp = fopen(file_path, "a");
    if (fp) {
        va_list args;
        va_start(args, fmt);
        fprintf(fp, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
        vfprintf(fp, fmt, args);
        fprintf(fp, "\n");
        va_end(args);
        fclose(fp);
    }
}
//===========================================================================

//======================== scenario.CSV 파일 관련 =======================
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
#define PI 3.14159265358979323846
#define DEG2RAD(x) ((x) * PI / 180.0)
#define RAD2DEG(x) ((x) * 180.0 / PI)

double calculate_bearing(double lat1, double lon1, double lat2, double lon2) {	// 두 좌표(위/경도) 사이의 방위각(0~360도) 계산
    double y = sin(DEG2RAD(lon2 - lon1)) * cos(DEG2RAD(lat2));
    double x = cos(DEG2RAD(lat1)) * sin(DEG2RAD(lat2)) -
               sin(DEG2RAD(lat1)) * cos(DEG2RAD(lat2)) * cos(DEG2RAD(lon2 - lon1));
    double bearing = atan2(y, x);
    return fmod((RAD2DEG(bearing) + 360.0), 360.0);
}

int bearing_to_dircode(double bearing) {	// 방위각을 8방향 CVIBDirCode로 변환
    // CVIBDirCode: 북(10), 동(20), 남(30), 서(40), 북동(50), 남동(60), 남서(70), 북서(80)
    // 8방위: 360도를 45도로 분할 (각 구간의 중심을 기준으로 +/- 22.5도)
	// todo. system_set_ptr 내부에 저장된 CVIBDirCode를 보고 범위 결정 자동화
    if (bearing >= 337.5 || bearing < 22.5)  return 10; // 북
    if (bearing >= 22.5  && bearing < 67.5)  return 50; // 북동
    if (bearing >= 67.5  && bearing < 112.5) return 20; // 동
    if (bearing >= 112.5 && bearing < 157.5) return 60; // 남동
    if (bearing >= 157.5 && bearing < 202.5) return 30; // 남
    if (bearing >= 202.5 && bearing < 247.5) return 70; // 남서
    if (bearing >= 247.5 && bearing < 292.5) return 40; // 서
    if (bearing >= 292.5 && bearing < 337.5) return 80; // 북서
    return 0;
}

/*
	idx: ApproachTrafficInfo의 번호, i값
	cal_type: 방향코드 추정 로직 타입
	output_type: 추정할 정보 타입(1:HO 진입 방향 추정, 2:HO 진출 방향 추정, 3:RO 진입 방향 추정)
*/
int estimation_direction_code(uint8_t idx, uint8_t cal_type, uint8_t output_type) {	// Waypoint의 GPS 값을 가지고 객체의 진입/진출 방향을 추정하는 코드 (CVIBDirCode 추가되면 사용 축소 가능)
	switch (cal_type) {	// todo. type 코드에 따라 각각 다른 로직 적용하기
		case 1: {	// Waypoint의 초기 2점과, 마지막 2점을 각각 벡터화해서 진입/진출 방향 추정 방식
			int output = 0;	// 뽑아낼 방향코드
			if (output_type == 1) {	// HO 진입방향 추정 요청
				int ho_wp_count = message_data_ptr->ApproachTrafficInfo[idx].HostObject.Num_Of_HO_WayPoint;
				if (ho_wp_count >= 2) {	// HO 진입 방향 추정: 진입 방향 헤딩 계산
					double lat1 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[0].lat;
					double lon1 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[0].lon;
					double lat2 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[1].lat;
					double lon2 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[1].lon;
					double bearing = calculate_bearing(lat1, lon1, lat2, lon2);
					double entry_bearing = fmod(bearing + 180.0, 360.0);	// 진입 방향은 진행 방향(Bearing)의 반대편 도로니까
					output = bearing_to_dircode(entry_bearing);
					// todo. 추정 결과 로깅
				}
			}
			if (output_type == 2) {	// HO 진출방향 추정 요청
				int ho_wp_count = message_data_ptr->ApproachTrafficInfo[idx].HostObject.Num_Of_HO_WayPoint;
				if (ho_wp_count >= 2) {	// HO 진출 방향 추정: 마지막 순간의 헤딩 계산
					int last_idx = ho_wp_count - 1;
					int prev_idx = ho_wp_count - 2;
					double lat1 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[prev_idx].lat;
					double lon1 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[prev_idx].lon;
					double lat2 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[last_idx].lat;
					double lon2 = message_data_ptr->ApproachTrafficInfo[idx].HostObject.WayPoint[last_idx].lon;
					double bearing = calculate_bearing(lat1, lon1, lat2, lon2);
					output = bearing_to_dircode(bearing);
					// todo. 추정 결과 로깅
				}
			}
			if (output_type == 3) {	// RO 진입방향 추정 요청
				int ro_wp_count = message_data_ptr->ApproachTrafficInfo[idx].RemoteObject.Num_Of_RO_WayPoint;
				if (ro_wp_count >= 2) {	// RO 진입 방향 추정: 진입 방향 헤딩 계산
					double lat1 = message_data_ptr->ApproachTrafficInfo[idx].RemoteObject.WayPoint[0].lat;
					double lon1 = message_data_ptr->ApproachTrafficInfo[idx].RemoteObject.WayPoint[0].lon;
					double lat2 = message_data_ptr->ApproachTrafficInfo[idx].RemoteObject.WayPoint[1].lat;
					double lon2 = message_data_ptr->ApproachTrafficInfo[idx].RemoteObject.WayPoint[1].lon;
					double bearing = calculate_bearing(lat1, lon1, lat2, lon2);
					double entry_bearing = fmod(bearing + 180.0, 360.0);	// 진입 방향은 진행 방향(Bearing)의 반대편 도로니까
					output = bearing_to_dircode(entry_bearing);
					// todo. 추정 결과 로깅
				}
			}
			return output;
            break; }
		case 2: {	// 교차로의 중앙 GPS 값을 기준으로, 첫번째 혹은 마지막 Waypoint 값으로 진입/진출 방향 추정 방식
			int output = 0;	// 뽑아낼 방향코드
			// todo. 
			return output;
			break; }
		default:
			break;
	}
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
	// 기존 메시지값들 0으로 초기화 (객체 없으면 0번 인덱스의 메시지 뿌려야됨)
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
	memset(vms_command_ptr->led_msg, 0, sizeof(vms_command_ptr->led_msg));

	// 디버깅용 MsgCount랑 Timestamp 복사
	strcpy(vms_command_ptr->Timestamp, message_data_ptr->Timestamp);
    vms_command_ptr->MsgCount = message_data_ptr->MsgCount;

	// LED 제어용 HO 개수 업데이트
	vms_command_ptr->ho_count = message_data_ptr->Num_Of_ApproachTrafficInfo;

	for (int i = 0; i < message_data_ptr->Num_Of_ApproachTrafficInfo; i++) {	// 공유메모리의 message_data_ptr->ApproachTrafficInfo 순회하면서 시나리오랑 매칭
		int PETGap_i = (int)(message_data_ptr->ApproachTrafficInfo[i].PET_Threshold - message_data_ptr->ApproachTrafficInfo[i].PET);	// RO가 없으면 걍 큰값으로 남겠지머

		int ho_entry_i = message_data_ptr->ApproachTrafficInfo[i].HostObject.CVIBDirCode;

		int ho_egress_i = 0;
		if (message_data_ptr->ApproachTrafficInfo[i].HostObject.Num_Of_HO_WayPoint > 0) {	// HO의 Waypoint가 있으면
			ho_egress_i = estimation_direction_code(i, 1, 2);
		}	// Waypoint 없으면 ho_egress_i는 걍 0으로 두기

		int ro_entry_i = 0;
		if (message_data_ptr->ApproachTrafficInfo[i].PET != -1) {	// RO가 존재하면
			ro_entry_i = estimation_direction_code(i, 1, 3);
		} // RO가 없으면 ro_entry_i는 걍 0으로 두기

		int speed_i = 0;
		if (message_data_ptr->ApproachTrafficInfo[i].HostObject.Num_Of_HO_WayPoint > 0) {	// HO Waypoint가 있으면, 첫번째 포인트의 속도값 긁어오기
            speed_i = (int)(message_data_ptr->ApproachTrafficInfo[i].HostObject.WayPoint[0].speed);
        }	// 없으면 0으로 두기

		vms_command_ptr->led_msg[i][0] = ho_entry_i;	// LED 제어를 위한 HO 진입/진출 경로 전달
		vms_command_ptr->led_msg[i][1] = ho_egress_i;

		Log_data(LOG_TYPE_VMS, "진입/진출 방향 추정 결과\n"
								"   MsgCount: %d | ApproachTrafficInfo no: %d\n"
								"   HO 진입: %d\n"
								"   HO 진출: %d\n"
								"   RO 진입: %d"
								, vms_command_ptr->MsgCount, i
								, ho_entry_i, ho_egress_i, ro_entry_i);

		for (int j = 0; j < g_scenario_count; j++) {	// 시나리오랑 매칭
            SCENARIO_ROW *row_j = &g_scenario_table[j];
            if (row_j->ho_entry == convert_dircode_to_count(ho_entry_i) && 
                row_j->ho_egress == convert_dircode_to_count(ho_egress_i) && 
                row_j->ro_entry == convert_dircode_to_count(ro_entry_i)) {	// 지금 시나리오 csv에서는 숫자순서대로 1234로 들어가있음
                
				// logger_log(LOG_LEVEL_DEBUG, "시나리오 CSV 파일과 매칭한 결과: \n"
				// 							"매칭 idx: %d\n"
				// 							"ho_entry_i = %d, ho_egress_i = %d, ro_entry_i = %d"
				// 							, j+1, ho_entry_i, ho_egress_i, ro_entry_i);
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
	Log_data(LOG_TYPE_VMS, "표출 정보 생성: MsgCount: %d, Timestamp: %s\n"
								"   북쪽(북동쪽) 도로, 교차로 진입 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n"
								"   북쪽(북동쪽) 도로, 가드레일 / 지주 타입: msg:%d, spd:%d, pet_gap:%d\n"
								"   북쪽(북동쪽) 도로, 교차로 진출 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n\n"
							
								"   동쪽(남동쪽) 도로, 교차로 진입 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n"
								"   동쪽(남동쪽) 도로, 가드레일 / 지주 타입: msg:%d, spd:%d, pet_gap:%d\n"
								"   동쪽(남동쪽) 도로, 교차로 진출 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n\n"
							
								"   남쪽(남서쪽) 도로, 교차로 진입 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n"
								"   남쪽(남서쪽) 도로, 가드레일 / 지주 타입: msg:%d, spd:%d, pet_gap:%d\n"
								"   남쪽(남서쪽) 도로, 교차로 진출 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n\n"
							
								"   서쪽(북서쪽) 도로, 교차로 진입 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n"
								"   서쪽(북서쪽) 도로, 가드레일 / 지주 타입: msg:%d, spd:%d, pet_gap:%d\n"
								"   서쪽(북서쪽) 도로, 교차로 진출 방향 볼라드: msg:%d, spd:%d, pet_gap:%d\n\n"

								"   표출장치 밝기 값: %f\n\n"
							, vms_command_ptr->MsgCount, vms_command_ptr->Timestamp

							, vms_command_ptr->n_in_msg[0], vms_command_ptr->n_in_msg[1], vms_command_ptr->n_in_msg[2]
							, vms_command_ptr->n_load_msg[0], vms_command_ptr->n_load_msg[1], vms_command_ptr->n_load_msg[2]
							, vms_command_ptr->n_out_msg[0], vms_command_ptr->n_out_msg[1], vms_command_ptr->n_out_msg[2]

							, vms_command_ptr->e_in_msg[0], vms_command_ptr->e_in_msg[1], vms_command_ptr->e_in_msg[2]
							, vms_command_ptr->e_load_msg[0], vms_command_ptr->e_load_msg[1], vms_command_ptr->e_load_msg[2]
							, vms_command_ptr->e_out_msg[0], vms_command_ptr->e_out_msg[1], vms_command_ptr->e_out_msg[2]
						
							, vms_command_ptr->s_in_msg[0], vms_command_ptr->s_in_msg[1], vms_command_ptr->s_in_msg[2]
							, vms_command_ptr->s_load_msg[0], vms_command_ptr->s_load_msg[1], vms_command_ptr->s_load_msg[2]
							, vms_command_ptr->s_out_msg[0], vms_command_ptr->s_out_msg[1], vms_command_ptr->s_out_msg[2]
						
							, vms_command_ptr->w_in_msg[0], vms_command_ptr->w_in_msg[1], vms_command_ptr->w_in_msg[2]
							, vms_command_ptr->w_load_msg[0], vms_command_ptr->w_load_msg[1], vms_command_ptr->w_load_msg[2]
							, vms_command_ptr->w_out_msg[0], vms_command_ptr->w_out_msg[1], vms_command_ptr->w_out_msg[2]
						
							, vms_command_ptr->brightness);
}
//============================ TCP 연결 관리 =============================
bool host_connect() {   // 클라이언트로써 연결 시도
	if (connection_status_ptr->ig_server_conn) { return true; }
	logger_log(LOG_LEVEL_DEBUG, "IG_Server] Try to Connect IP:%s, Port:%d"
            , system_set_ptr->ig_server_ip
            , system_set_ptr->ig_server_port);
	HandleIndex = CommInit(CONNECT, &system_set_ptr->ig_server_ip, system_set_ptr->ig_server_port, true);
	if (HandleIndex == -1) {
		logger_log(LOG_LEVEL_ERROR, "IG_Server] Connect Fail, IP:%s, Port:%d", &system_set_ptr->ig_server_ip, system_set_ptr->ig_server_port);
		connection_status_ptr->ig_server_conn = false;
		return false;
	} else {
		logger_log(LOG_LEVEL_DEBUG, "IG_Server] Connect Success, IP:%s, Port:%d", &system_set_ptr->ig_server_ip, system_set_ptr->ig_server_port);
		connection_status_ptr->ig_server_conn = true;
		last_keep_alive_time = time(NULL);
    }
	usleep(100000);  //100ms
	return true;
}
//============================ TCP 수신 함수 =============================
#define MAX_RECV_BUFFER_SIZE (1024 * 16)	// 한번에 최대 16kb
static uint8_t g_recv_buffer[MAX_RECV_BUFFER_SIZE * 10];	// 글로벌 버퍼, 대충 MAX_RECV_BUFFER_SIZE의 10배
static size_t g_buffer_len = 0;

static void Analysis_Packet(cJSON* json_root) {	// IG-Server에서 받은 cJSON 객체 파싱 및 공유메모리에 업데이트
    const cJSON* json_MsgCount = cJSON_GetObjectItemCaseSensitive(json_root, "MsgCount");
	if (cJSON_IsNumber(json_MsgCount)) {
		message_data_ptr->MsgCount = json_MsgCount->valueint;
		Log_data(LOG_TYPE_SHM, "MsgCount: %d", message_data_ptr->MsgCount);
		// logger_log(LOG_LEVEL_DEBUG, "\nMsgCount: %d", message_data_ptr->MsgCount);
	}
	const cJSON* json_Timestamp = cJSON_GetObjectItemCaseSensitive(json_root, "Timestamp");
    if (cJSON_IsString(json_Timestamp)) {
		strcpy(message_data_ptr->Timestamp, json_Timestamp->valuestring);
		Log_data(LOG_TYPE_SHM, "Timestamp: %s", message_data_ptr->Timestamp);
	}

    cJSON* json_ApproachTrafficInfoList = cJSON_GetObjectItem(json_root, "ApproachTrafficInfoList");	// ApproachTrafficInfoList 배열 파싱
    if (cJSON_IsArray(json_ApproachTrafficInfoList)) {
		Log_data(LOG_TYPE_SHM, "ApproachTrafficInfoList");
        cJSON* json_ApproachTrafficInfo_i = NULL;
		int traffic_info_index = 0;
        cJSON_ArrayForEach(json_ApproachTrafficInfo_i, json_ApproachTrafficInfoList) {	// 각 ApproachTrafficInfo 순회하기
			cJSON* json_ApproachTrafficInfo = cJSON_GetObjectItem(json_ApproachTrafficInfo_i, "ApproachTrafficInfo");
			if (cJSON_IsObject(json_ApproachTrafficInfo)) {	// json_ApproachTrafficInfo 내부에 데이터가 비어있지 않으면 파싱하기
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

				Log_data(LOG_TYPE_SHM, " ApproachTrafficInfo no:%d", traffic_info_index);
				Log_data(LOG_TYPE_SHM, "   ConflictPos_Lat: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].ConflictPos.lat);
				Log_data(LOG_TYPE_SHM, "   ConflictPos_Lon: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].ConflictPos.lon);
				Log_data(LOG_TYPE_SHM, "   PET: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].PET);

				const cJSON* json_PET_Threshold = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "PET_Threshold");
				if (cJSON_IsNumber(json_PET_Threshold)) {
					message_data_ptr->ApproachTrafficInfo[traffic_info_index].PET_Threshold = json_PET_Threshold->valuedouble;
					Log_data(LOG_TYPE_SHM, "   PET_Threshold: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].PET_Threshold);
				}

				const cJSON* json_HostObject = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "HostObject");	// HO 파싱
				if (cJSON_IsObject(json_HostObject)) {
					const cJSON* json_HO_ObjectType = cJSON_GetObjectItemCaseSensitive(json_HostObject, "ObjectType");
					const cJSON* json_HO_ObjectID = cJSON_GetObjectItemCaseSensitive(json_HostObject, "ObjectID");
					if (cJSON_IsString(json_HO_ObjectType) && cJSON_IsString(json_HO_ObjectID)) {
						strcpy(message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectType, json_HO_ObjectType->valuestring);
						strcpy(message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectID, json_HO_ObjectID->valuestring);

						Log_data(LOG_TYPE_SHM, "   HostObject");
						Log_data(LOG_TYPE_SHM, "      ObjectType: %s", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectType);
						Log_data(LOG_TYPE_SHM, "      ObjectID: %s", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.ObjectID);
					}
					/*
					todo. 위치 이동 후 주석 제거, 아래꺼 삭제
					const cJSON* json_HostObj_CVIBDirCode = cJSON_GetObjectItemCaseSensitive(json_HostObject, "CVIBDirCode");
					if (cJSON_IsNumber(json_HostObj_CVIBDirCode)) {
						message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.CVIBDirCode = json_HostObj_CVIBDirCode->valueint;
						Log_data(LOG_TYPE_SHM, "      CVIBDirCode: %d", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.CVIBDirCode);
					}
					*/
					const cJSON* json_HostObj_CVIBDirCode = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "CVIBDirCode");
					if (cJSON_IsNumber(json_HostObj_CVIBDirCode)) {
						message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.CVIBDirCode = json_HostObj_CVIBDirCode->valueint;
						Log_data(LOG_TYPE_SHM, "      CVIBDirCode: %d", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.CVIBDirCode);
					}

					const cJSON* json_HO_WayPointList = cJSON_GetObjectItemCaseSensitive(json_HostObject, "WayPointList");
					if (cJSON_IsArray(json_HO_WayPointList)) {
						Log_data(LOG_TYPE_SHM, "      WayPointList");
						cJSON* json_WayPoint_i = NULL;
						int wayPoint_index = 0;
						cJSON_ArrayForEach(json_WayPoint_i, json_HO_WayPointList) {	// 각 WayPoint 순회하기
							const cJSON* json_WayPoint = cJSON_GetObjectItemCaseSensitive(json_WayPoint_i, "WayPoint");
							if (json_WayPoint){	// "WayPointList":[]처럼 빈 리스트일 수 있으니까
								const cJSON* json_WayPoint_lat = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lat");
								const cJSON* json_WayPoint_lon = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lon");
								const cJSON* json_WayPoint_speed = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "speed");
								if (cJSON_IsNumber(json_WayPoint_lat) && cJSON_IsNumber(json_WayPoint_lon) && cJSON_IsNumber(json_WayPoint_speed)) {
									message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].lat = json_WayPoint_lat->valuedouble;
									message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].lon = json_WayPoint_lon->valuedouble;
									message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].speed = json_WayPoint_speed->valuedouble;

									Log_data(LOG_TYPE_SHM, "            WayPoint no %d", wayPoint_index);
									Log_data(LOG_TYPE_SHM, "               Lat: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].lat);
									Log_data(LOG_TYPE_SHM, "               Lon: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].lon);
									Log_data(LOG_TYPE_SHM, "               Speed: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.WayPoint[wayPoint_index].speed);
								}
							}
							wayPoint_index++;
						}
						message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.Num_Of_HO_WayPoint = wayPoint_index;
						Log_data(LOG_TYPE_SHM, "      Num_Of_HO_WayPoint: %d", message_data_ptr->ApproachTrafficInfo[traffic_info_index].HostObject.Num_Of_HO_WayPoint);
					}
				}
				const cJSON* json_RemoteObject = cJSON_GetObjectItemCaseSensitive(json_ApproachTrafficInfo, "RemoteObject");	// RO 파싱
				if (json_RemoteObject && cJSON_IsObject(json_RemoteObject)) {	// 얜 없을수도있음
					const cJSON* json_RO_ObjectType = cJSON_GetObjectItemCaseSensitive(json_RemoteObject, "ObjectType");
					const cJSON* json_RO_ObjectID = cJSON_GetObjectItemCaseSensitive(json_RemoteObject, "ObjectID");
					if (cJSON_IsString(json_RO_ObjectType) && cJSON_IsString(json_RO_ObjectID)) {
						strcpy(message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectType, json_RO_ObjectType->valuestring);
						strcpy(message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectID, json_RO_ObjectID->valuestring);

						Log_data(LOG_TYPE_SHM, "   RemoteObject");
						Log_data(LOG_TYPE_SHM, "      ObjectType: %s", message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectType);
						Log_data(LOG_TYPE_SHM, "      ObjectID: %s", message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.ObjectID);
					}
					/*
					todo. 웨이즈원이 추가해주면 주석 제거하기
					const cJSON* json_RemoteObj_CVIBDirCode = cJSON_GetObjectItemCaseSensitive(RemoteObject, "CVIBDirCode");
					if (cJSON_IsNumber(json_RemoteObj_CVIBDirCode)) {
						message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.CVIBDirCode = json_RemoteObj_CVIBDirCode->valueint;
						Log_data(LOG_TYPE_SHM, "      CVIBDirCode: %d", message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.CVIBDirCode);
					}
					*/
					const cJSON* json_RO_WayPointList = cJSON_GetObjectItemCaseSensitive(json_RemoteObject, "WayPointList");
					if (cJSON_IsArray(json_RO_WayPointList)) {
						Log_data(LOG_TYPE_SHM, "      WayPointList");
						cJSON* json_WayPoint_i = NULL;
						int wayPoint_index = 0;
						cJSON_ArrayForEach(json_WayPoint_i, json_RO_WayPointList) {	// 각 WayPoint 순회하기
							const cJSON* json_WayPoint = cJSON_GetObjectItemCaseSensitive(json_WayPoint_i, "WayPoint");
							if (json_WayPoint){	// "WayPointList":[]처럼 빈 리스트일 수 있으니까
								const cJSON* json_WayPoint_lat = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lat");
								const cJSON* json_WayPoint_lon = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "lon");
								const cJSON* json_WayPoint_speed = cJSON_GetObjectItemCaseSensitive(json_WayPoint, "speed");
								if (cJSON_IsNumber(json_WayPoint_lat) && cJSON_IsNumber(json_WayPoint_lon) && cJSON_IsNumber(json_WayPoint_speed)) {
									message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].lat = json_WayPoint_lat->valuedouble;
									message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].lon = json_WayPoint_lon->valuedouble;
									message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].speed = json_WayPoint_speed->valuedouble;

									Log_data(LOG_TYPE_SHM, "            WayPoint no %d", wayPoint_index);
									Log_data(LOG_TYPE_SHM, "               Lat: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].lat);
									Log_data(LOG_TYPE_SHM, "               Lon: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].lon);
									Log_data(LOG_TYPE_SHM, "               Speed: %f", message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.WayPoint[wayPoint_index].speed);
								}
							}
							wayPoint_index++;
						}
						message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.Num_Of_RO_WayPoint = wayPoint_index;
						Log_data(LOG_TYPE_SHM, "      Num_Of_RO_WayPoint: %d", message_data_ptr->ApproachTrafficInfo[traffic_info_index].RemoteObject.Num_Of_RO_WayPoint);
					}
				}
				traffic_info_index++;
			}
        }
		message_data_ptr->Num_Of_ApproachTrafficInfo = traffic_info_index;
		Log_data(LOG_TYPE_SHM, "Num_Of_ApproachTrafficInfo: %d", message_data_ptr->Num_Of_ApproachTrafficInfo);
    }
	calc_vms_command();	// 파싱 후 VMS 제어용 정보 공유메모리에 업데이트하기
}

void append_to_global_buffer(uint8_t* new_data, size_t length) {	// 수신한 데이터를 글로벌 버퍼에 업로드
    if (g_buffer_len + length > MAX_RECV_BUFFER_SIZE) {
        logger_log(LOG_LEVEL_ERROR, "IG_Server] 전역 버퍼 오버플로우: 버퍼 초기화.\n");
        g_buffer_len = 0;
        return;
    }
    memcpy(&g_recv_buffer[g_buffer_len], new_data, length);
    g_buffer_len += length;
}
void remove_from_global_buffer(size_t remove_len) {	// 글로벌 버퍼에서 데이터 삭제	// todo. memmove로 지우는 방식에 문제가 있을수도 있음. 확인 필요
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
		int start_M_inx = 6;
        if (g_recv_buffer[start_M_inx] != 0x4D || g_recv_buffer[start_M_inx+1] != 0x73 ||
            g_recv_buffer[start_M_inx+2] != 0x67 || g_recv_buffer[start_M_inx+3] != 0x43 ||
            g_recv_buffer[start_M_inx+4] != 0x6F || g_recv_buffer[start_M_inx+5] != 0x75 ||
            g_recv_buffer[start_M_inx+6] != 0x6E || g_recv_buffer[start_M_inx+7] != 0x74) { // JSON 내용 중 "MsgCount" 위치 확인
            remove_from_global_buffer(1);   // 못찾았으면 1바이트 버리고 다음 루프에서 재검사
            continue;
        } else {    // 헤더 찾음
            uint32_t msg_len;
            memcpy(&msg_len, &g_recv_buffer[0], sizeof(msg_len));
            if (g_buffer_len < msg_len + 4) {	// 길이만큼 안왔으면 다음 수신 기다리기
				break;
			}
            cJSON* json_root = cJSON_ParseWithLength((const char*)&g_recv_buffer[4], msg_len);
            if (json_root) {	// JSON 파싱에 성공했을 시
                Analysis_Packet(json_root);
				/* 디버깅용 수신 JSON 데이터 로깅 */
                char *json_string = cJSON_PrintUnformatted(json_root);
                if (json_string != NULL) {
					Log_data(LOG_TYPE_RAW, "%s", json_string);	// raw data 로깅
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
    BOOL bReturn = false;
	if (g_buffer_len >= MAX_RECV_BUFFER_SIZE) {
        logger_log(LOG_LEVEL_ERROR, "Receive buffer overflow. Resetting buffer.");	// todo. 이거 종종 오버플로우 나던데 확인 필요함
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
    } else if (bReturn == FALSE) {
        int err = GetLastCommError(); // CommData.c의 에러코드 확인
        // 연결이 끊어진 경우 (ReadnStream에서 0 리턴 시 DISCONNECTED 설정됨)
        if (err == DISCONNECTED || (nReadSize == 0 && err != TIME_OUT)) {
            logger_log(LOG_LEVEL_INFO, "IG_Server] Disconnected by Server/Network.");
            CommClose(HandleIndex);
            HandleIndex = -1;
            connection_status_ptr->ig_server_conn = false;
        }
	}
	process_parsing();  // 글로벌 버퍼에 쌓인 데이터 파싱 시도
}
//================================================================


//===================== 프로세스간 메시지 송/수신 =====================
void message_analy(BYTE *_pData) {  // 외부 프로세스에서 받은 데이터 파싱
	if(_pData[0] != (0x80 | IG_SERVER_Q_PID))
	{
		// printf("massege id %02x %02x err~\n", _pData[0], (0x80 | IG_SERVER_Q_PID));
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
		}
		if(st_5s_cnt++ >= 49)
		{
			st_5s_cnt = 0;
			if ((nowtime-last_keep_alive_time) >= 5) {	// 5초 이상 데이터가 안 들어오면
				logger_log(LOG_LEVEL_ERROR, "5초 이상 수신 데이터 없음. 소켓 해제 및 재연결 시도");
				connection_status_ptr->ig_server_conn = false;
			}
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
	// bool bReturn;

	if (logger_init("Logs/IG_Server_Manager_Log", 100) != 0) {	// 로거 테스트용 100mb
		logger_log(LOG_LEVEL_ERROR, "Logger init failed");
        exit(EXIT_FAILURE);
    }

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

	// bReturn = host_connect();	// IG-Server와 연결
	// logger_log(LOG_LEVEL_DEBUG, "IG_Server] 서버 리스닝 : %d", bReturn);

	nowtime = time(NULL);	// 재연결 타임아웃 검지를 위한 현재시간 초기화

	while (1)
	{
		usleep(50000);  //50ms
		if(connection_status_ptr->ig_server_conn == false) {
			if (HandleIndex != -1) {	// 재연결해야되는데 소켓이 살아있으면
                logger_log(LOG_LEVEL_INFO, "IG_Server] Cleaning up old socket handle: %d", HandleIndex);
                CommClose(HandleIndex);
                HandleIndex = -1;
            }
            static time_t last_retry = 0;
            time_t current_time = time(NULL);
			bool Return_re;
            if (current_time - last_retry > 3) { // 3초마다 재시도
                // printf("IG_Server] Retrying connection...\n");
                Return_re = host_connect();
				logger_log(LOG_LEVEL_DEBUG, "IG_Server] 서버 리스닝 : %d", Return_re);
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
