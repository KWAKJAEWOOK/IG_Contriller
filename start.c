/* start.c - Linux Version */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/global.h"

#define MAX_PATH_LEN 4096

/* 전역 설정 포인터 (shm_func.c 등에 정의된 변수와 연결) */
// extern SHM_SYSTEM_SET *system_set_ptr;

void Generate_Group_IP(char (*ip_list)[32], int count) {	// 기준 IP(0번 인덱스)를 바탕으로 count 개수만큼 IP를 1씩 증가시켜 배열에 채움
    if (count <= 1) return; // 1개 이하면 생성안함
    int ip[4];
    if (sscanf(ip_list[0], "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]) != 4) {	// 0번 인덱스(기준 IP) 파싱
        printf("START] Invalid Base IP format: %s\n", ip_list[0]);
        return;
    }
    for (int i = 1; i < count; i++) {	// 1번 인덱스부터 count-1번 인덱스까지 생성
        int temp_ip[4] = {ip[0], ip[1], ip[2], ip[3]};
        temp_ip[3] += i;	// 마지막 자리 증가
        for (int j = 3; j > 0; j--) {	// 자리올림 처리 (255.255.255.255 초과 시 로직은 생략, 일반적 서브넷 내 처리)
            if (temp_ip[j] > 255) {
                temp_ip[j-1] += temp_ip[j] / 256;
                temp_ip[j] %= 256;
            }
        }
        snprintf(ip_list[i], 32, "%d.%d.%d.%d", 
                 temp_ip[0], temp_ip[1], temp_ip[2], temp_ip[3]);	// 결과 배열에 저장
    }
    for (int i = 0; i < count; i++) {
        printf("M30 IP[%d]: %s\n", i, ip_list[i]);	// 디버깅용 출력
    }
}

int Load_System_Set()
{
    FILE *init_file;
    char para_name[64];
    char value[64];
    char szFileName[MAX_PATH_LEN];
    char exePath[MAX_PATH_LEN];
    char ini_buffer[256];

    // 1. 실행 중인 실행 파일(.out)의 경로 구하기 (Linux 방식)
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len != -1) {
        exePath[len] = '\0'; // Null-terminate
    } else {
        perror("readlink");
        return 0;
    }

    // 2. 실행 파일명 제거하고 디렉토리만 남기기 ("/home/user/bin/start.out" -> "/home/user/bin/")
    char *p = strrchr(exePath, '/');
    if (p) {
        *(p + 1) = '\0'; // 슬래시 다음을 잘라냄 (슬래시 포함)
    }

    // 3. 설정 파일 경로 완성
    snprintf(szFileName, sizeof(szFileName), "%sinit/config.ini", exePath);

    printf("START] Load %s \n", szFileName);

    if ((init_file = fopen(szFileName, "r")) == NULL) { // "rb" -> "r" (리눅스는 텍스트모드 구분 불필요)
        fprintf(stderr, "can't open %s, load INIT file \n", szFileName);
        return 0;
    }

    while (fgets(ini_buffer, sizeof(ini_buffer), init_file) != NULL) {
        memset(para_name, 0, sizeof(para_name));
        memset(value, 0, sizeof(value));

        // 파싱 (공백 기준 분리)
        if (sscanf(ini_buffer, "%s %s", para_name, value) != 2) continue;

        if (strcmp(para_name, "IG_Server_IP") == 0) {
            strncpy(system_set_ptr->ig_server_ip, value, 32);
        } else if (strcmp(para_name, "IG_Server_PORT") == 0) {
            system_set_ptr->ig_server_port = atoi(value);
        }

        else if (strcmp(para_name, "LED_IP") == 0) {
            strncpy(system_set_ptr->led_ip, value, 32);
        } else if (strcmp(para_name, "LED_PORT") == 0) {
            system_set_ptr->led_port = atoi(value);
        }

        /* M30 관련 설정 */
        else if (strcmp(para_name, "M30_N_IN_IP") == 0) {
			strncpy(&system_set_ptr->m30_n_in_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_N_IN_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_n_in_ip, count);
		}
		else if (strcmp(para_name, "M30_N_LOAD_IP") == 0) {
			strncpy(&system_set_ptr->m30_n_load_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_N_LOAD_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_n_load_ip, count);
		}
		else if (strcmp(para_name, "M30_N_OUT_IP") == 0) {
			strncpy(&system_set_ptr->m30_n_out_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_N_OUT_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_n_out_ip, count);
		}

		else if (strcmp(para_name, "M30_E_IN_IP") == 0) {
			strncpy(&system_set_ptr->m30_e_in_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_E_IN_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_e_in_ip, count);
		}
		else if (strcmp(para_name, "M30_E_LOAD_IP") == 0) {
			strncpy(&system_set_ptr->m30_e_load_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_E_LOAD_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_e_load_ip, count);
		}
		else if (strcmp(para_name, "M30_E_OUT_IP") == 0) {
			strncpy(&system_set_ptr->m30_e_out_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_E_OUT_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_e_out_ip, count);
		}

		else if (strcmp(para_name, "M30_S_IN_IP") == 0) {
			strncpy(&system_set_ptr->m30_s_in_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_S_IN_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_s_in_ip, count);
		}
		else if (strcmp(para_name, "M30_S_LOAD_IP") == 0) {
			strncpy(&system_set_ptr->m30_s_load_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_S_LOAD_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_s_load_ip, count);
		}
		else if (strcmp(para_name, "M30_S_OUT_IP") == 0) {
			strncpy(&system_set_ptr->m30_s_out_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_S_OUT_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_s_out_ip, count);
		}

		else if (strcmp(para_name, "M30_W_IN_IP") == 0) {
			strncpy(&system_set_ptr->m30_w_in_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_W_IN_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_w_in_ip, count);
		}
		else if (strcmp(para_name, "M30_W_LOAD_IP") == 0) {
			strncpy(&system_set_ptr->m30_w_load_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_W_LOAD_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_w_load_ip, count);
		}
		else if (strcmp(para_name, "M30_W_OUT_IP") == 0) {
			strncpy(&system_set_ptr->m30_w_out_ip[0], value, 32);
		} else if (strcmp(para_name, "M30_W_OUT_COUNT") == 0) {
			int count = atoi(value);
			Generate_Group_IP(system_set_ptr->m30_w_out_ip, count);
		}
        
        else if (strcmp(para_name, "M30_PORT") == 0) {
            system_set_ptr->m30_port = atoi(value);
        }
    }

    fclose(init_file);
    return 1;
}

/**
 * 리눅스에서 프로세스 실행 함수
 * @param exeDir 실행 파일이 있는 디렉토리
 * @param exeName 실행할 파일 이름 (예: "SAFETYCON_Manager.out")
 */
static void start_linux_process(const char *exeDir, const char *exeName) {
    char cmd[1024];

    // ./SAFETYCON_Manager.out 형식으로 실행
    // nohup을 사용하면 터미널이 닫혀도 계속 실행되게 할 수 있음: "nohup ./%s > /dev/null 2>&1 &"
    
    snprintf(cmd, sizeof(cmd), "cd \"%s\" && ./%s &", exeDir, exeName);
    
    int ret = system(cmd);
    if (ret == -1) {
        fprintf(stderr, "Failed to execute: %s\n", cmd);
    } else {
        printf("Started: %s\n", exeName);
    }
    
    usleep(100000); // 100ms 지연
}

int main(int argc, char *argv[])
{
    int testMode = 0;
    if( argc >= 2 ) {
        if( argv[1][0] == 'H' || argv[1][0] == 'h' ) {
            // bHexFlag = 1;
        }
        else if ( argv[1][0] == 'T' || argv[1][0] == 't') {
            printf("START] TEST MODE!!\n");
            testMode = 1;
        }
    }

    // 1. 공유 메모리 생성 및 연결
    if (shm_all_create() == FALSE) { // create가 실패하면 이미 존재할 수 있으니 open 시도 or 로직 점검
        printf("START] shm_all_create() failed (Maybe already exists or Permission denied)\n");
        // Linux에서는 create가 실패해도 attach를 시도해보는 것이 좋음, 여기선 원본 흐름 유지
    }
    
    if (shm_all_open() == FALSE) {
        printf("START] shm_all_open() failed\n");
        exit(-1);
    }

    // 2. 메시지 큐 생성 및 연결
    if(msg_all_create() == FALSE) {
         printf("START] msg_all_create() failed\n");
    }
    
    // 3. 설정 파일 로드
    // 시스템 설정 포인터가 공유메모리에 연결된 후 실행해야 함
    if (Load_System_Set() <= 0 ) {
        printf("START] Load_System_Set failed\n");
        exit(-1);
    }

    // 4. 실행 파일 경로 구하기
    char exeDir[MAX_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", exeDir, sizeof(exeDir) - 1);
    if (len != -1) exeDir[len] = '\0';
    
    char *p = strrchr(exeDir, '/');
    if (p) *p = '\0'; // 디렉토리 경로만 남김

    printf("Working Directory: %s\n", exeDir);

    // 5. 프로세스 순차 실행 (.out 확장자 사용)
    
    start_linux_process(exeDir, "IG_Server_Manager.out");
    start_linux_process(exeDir, "LED_Manager.out");
    start_linux_process(exeDir, "M30_Manager.out");
    start_linux_process(exeDir, "Connection_Manager.out");

    // 6. 버전 파일 생성 (modifyYYYYMMDD)
    char system_version[64];
    char szFileName[128];
    FILE *fs;

    sprintf(system_version, "modify20250916");

    // 기존 파일 삭제
    system("rm -f ./modify*"); // -Rf 대신 -f 사용 권장

    memset(szFileName, 0, sizeof(szFileName));
    sprintf(szFileName, "./%s", system_version);
    
    if( (fs = fopen( szFileName, "w")) == NULL) { // "wt" -> "w"
        printf("START] can't create modifyFile: %s\n", szFileName);
    } else {
        fclose(fs);
    }

    printf("START] All processes started. Exiting start.out wrapper.\n");
    
    // start 프로세스는 자식들을 실행시키고 종료합니다. 
    exit(0);
}