/* watchdog.c - Process Monitor & Restarter */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/logger.h"

// 감시 대상 프로세스 구조체 정의
typedef struct {
    int pid_idx;            // shm_type.h의 ENUM (예: IG_SERVER_MANAGER_PID)
    char name[64];          // 로그 표시용 이름
    char binary_name[64];   // 실행 파일명
} PROCESS_INFO;

// 감시할 프로세스 목록
PROCESS_INFO target_processes[] = {
    { IG_SERVER_MANAGER_PID, "IG_Server",       "IG_Server_Manager.out" },
    { LED_MANAGER_PID,       "LED_Manager",     "LED_Manager.out" },
    { M30_MANAGER_PID,       "M30_Manager",     "M30_Manager.out" },
    { CONNECTION_MANAGER_PID,"Conn_Manager",    "Connection_Manager.out" },
    { BH1750_Manager_PID,    "BH1750_Manager",  "BH1750_Manager.out" },
};

#define TARGET_COUNT (sizeof(target_processes) / sizeof(PROCESS_INFO))

volatile int g_running = 1;

void handle_sigint(int sig) {
    g_running = 0;
    printf("\nWatchdog Terminating... (Signal: %d)\n", sig);
}

// 프로세스 실행 함수
void restart_process(const char* binary_name) {
    char cmd[256];
    // 백그라운드 실행 & (nohup 사용 권장)
    snprintf(cmd, sizeof(cmd), "./%s &", binary_name);
    
    int ret = system(cmd);
    if (ret == -1) {
        logger_log(LOG_LEVEL_ERROR, "Watchdog] Failed to restart %s", binary_name);
    } else {
        logger_log(LOG_LEVEL_WARN, "Watchdog] Restarted process: %s", binary_name);
    }
}

int main() {
    // 1. 로거 초기화
    if (logger_init("Logs/Watchdog_Log", 50) != 0) {
        printf("Watchdog Logger init failed\n");
    }
    logger_log(LOG_LEVEL_INFO, "Watchdog Process Started.");

    // 2. 공유메모리 연결 (PID 확인용)
    if (shm_all_open() == FALSE) {
        logger_log(LOG_LEVEL_ERROR, "Watchdog] shm_all_open() failed. Waiting for SHM creation...");
        // 공유메모리가 생성될 때까지 대기할 수도 있지만, 보통 start.c가 먼저 실행되므로 실패하면 종료
        exit(-1);
    }

    signal(SIGINT, handle_sigint);

    // Watchdog 자신은 공유메모리에 PID 등록 안함 (자신을 감시할 필요는 없으므로)
    // 혹은 별도 인덱스를 파서 등록해도 됨.

    while (g_running) {
        for (int i = 0; i < TARGET_COUNT; i++) {
            int idx = target_processes[i].pid_idx;
            pid_t pid = proc_shm_ptr->pid[idx];
            char *pname = target_processes[i].name;
            char *pbinary = target_processes[i].binary_name;

            int need_restart = 0;

            if (pid <= 0) {
                // PID가 0 이하이면 실행되지 않았거나 초기화된 상태
                need_restart = 1;
            } else {
                // kill(pid, 0)은 신호를 보내지 않고 프로세스 존재 여부만 확인 (0: 존재, -1: 에러)
                if (kill(pid, 0) == -1) {
                    if (errno == ESRCH) { 
                        // 프로세스가 존재하지 않음 (죽음)
                        logger_log(LOG_LEVEL_ERROR, "Watchdog] Process DEAD detected: %s (PID: %d)", pname, pid);
                        need_restart = 1;
                        
                        // 좀비 프로세스 방지 (혹시 system()으로 실행된 자식일 경우)
                        waitpid(pid, NULL, WNOHANG);
                        
                        // SHM PID 초기화
                        proc_shm_ptr->pid[idx] = 0;
                    }
                }
            }

            if (need_restart) {
                logger_log(LOG_LEVEL_INFO, "Watchdog] Try to start: %s", pname);
                restart_process(pbinary);
                sleep(1); // 재실행 후 PID 등록될 시간 잠깐 부여
            }
        }

        sleep(3); // 3초 주기로 감시
    }

    if (shm_close() != 0) {
        printf("Watchdog] shm_close failed\n");
    }
    
    return 0;
}