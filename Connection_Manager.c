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

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/logger.h"

// Ctrl+C 종료 핸들러
void handle_sigint(int sig) {
    printf("\nConnection_Manager Terminating... (Signal: %d)\n", sig);
    if (shm_close() != 0) {
        printf("shm_close() failed\n");
    }
    exit(0);
}

int main()
{
    if (shm_all_open() == false) {
        printf("CONN_Mgr] shm_all_open() failed\n");
        // 실패하더라도 로그 남기고 종료하거나, 재시도 로직 필요 (여기선 종료)
        exit(-1);
    }

    // 2. 메시지 큐 연결
    if (msg_all_open() == false) {
        printf("CONN_Mgr] msg_all_open() failed\n");
        exit(-1);
    }

    // 3. 종료 시그널 등록
    signal(SIGINT, handle_sigint);

    // 4. 프로세스 등록 (공유메모리에 PID 저장)
    if (proc_shm_ptr != NULL) {
        proc_shm_ptr->pid[CONNECTION_MANAGER_PID] = getpid();
        proc_shm_ptr->status[CONNECTION_MANAGER_PID] = 'S'; // 'S': Start/Running
    }

    // 5. 로거 초기화
    if (logger_init("Logs/Connection_Manager_Log", 100) != 0) {
        printf("Logger init failed\n");
    }
    logger_log(LOG_LEVEL_INFO, "Connection Manager Start.");

    // 6. 메인 루프
    while (1) {
        // 주기적으로 수행할 작업 (예: 1초마다 상태 체크)
        usleep(1000000); // 1초 대기

        // 예시: 공유메모리에서 다른 프로세스들이 살아있는지 확인하거나 상태 로그 출력
        // (현재는 빈 루프로 유지하여 프로세스가 죽지 않게 함)
        
        // 추후 센터 통신 로직 추가 위치
    }

    // 정상 종료 처리 (루프 탈출 시)
    logger_cleanup();
    shm_close();

    return 0;
}