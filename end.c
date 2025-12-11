#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

/* Linux Specific Headers */
#include <sys/types.h>
#include <sys/msg.h>
#include <signal.h> // kill(), SIGKILL
#include <dirent.h> // opendir(), readdir()

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"

// 외부 변수 선언 (shm_func.c)
// extern SHM_PROC_DATA *proc_shm_ptr; 

/**
 * 이름으로 프로세스 죽이기 (Linux /proc 탐색 방식)
 * @param procName 종료할 프로세스 이름 (부분 일치 허용)
 */
void kill_by_name(const char *procName) {
    DIR *dir;
    struct dirent *entry;
    
    // /proc 디렉토리 열기
    if (!(dir = opendir("/proc"))) {
        perror("opendir /proc failed");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        // 디렉토리 이름이 숫자가 아니면 건너뜀 (PID가 아닌 항목들)
        if (!isdigit(*entry->d_name)) continue;

        pid_t pid = atoi(entry->d_name);
        char filepath[256];
        char buf[256];
        
        // /proc/[PID]/cmdline 파일을 읽어서 프로세스 실행 명령어를 가져옴
        snprintf(filepath, sizeof(filepath), "/proc/%d/cmdline", pid);
        FILE *fp = fopen(filepath, "r");
        
        if (fp) {
            // cmdline은 null 문자로 구분되어 있으나, 첫 번째 문자열(실행파일 경로)만 읽어도 충분
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                // procName이 cmdline에 포함되어 있는지 확인
                if (strstr(buf, procName) != NULL) {
                    // 자기 자신(end.out)은 죽이지 않도록 보호 (선택 사항)
                    if (pid != getpid()) {
                        if (kill(pid, SIGKILL) == 0) {
                            printf("Killed %s (PID=%d)\n", procName, pid);
                        } else {
                            // 이미 죽었거나 권한 문제 등
                            fprintf(stderr, "Failed to kill %s (PID=%d): %s\n", procName, pid, strerror(errno));
                        }
                    }
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
}

int main(int argc, char *argv[])
{
    int i;

    // 1. 공유 메모리 연결 (기존 정보 읽기 위함)
    if( shm_all_open() == FALSE ) {
        printf("shm_all_open() failed. (Maybe memory not created yet)\n");
        // 메모리가 없어도 이름으로 죽이는 작업은 수행하도록 exit하지 않고 진행할 수도 있음
        // 공유메모리 ID를 알아야 삭제가 가능하므로 실패 시 cleanup 불가할 수 있음
    }

    // 2. 공유 메모리에 등록된 PID를 기반으로 프로세스 종료
    if (proc_shm_ptr != NULL) {
        for (i = 0; i < MAX_PROCESS; i++) {
            pid_t pid = (pid_t)proc_shm_ptr->pid[i];
            
            // PID가 유효하고(>0), 자기 자신이 아니면 종료 시도
            if (pid > 0 && pid != getpid()) {
                // kill(pid, 0)으로 프로세스 존재 확인 가능
                if (kill(pid, SIGKILL) == 0) {
                    printf("Terminated registered PID %d (Index %d)\n", pid, i);
                } else {
                    if (errno != ESRCH) { // ESRCH: No such process (이미 없음)
                        printf("Failed to kill PID %d: %s\n", pid, strerror(errno));
                    }
                }
                // PID 초기화
                proc_shm_ptr->pid[i] = 0;
            }
        }
    }

    // 3. 이름으로 명시적 종료 (좀비 프로세스나 등록되지 않은 프로세스 정리)
    kill_by_name("watchdog.out");
    usleep(100000);
    kill_by_name("IG_Server_Manager.out");
    kill_by_name("LED_Manager.out");
    kill_by_name("M30_Manager.out");
    kill_by_name("Connection_Manager.out");
    kill_by_name("BH1750_Manager.out");

    printf("All processes termination sequence completed.\n");

    // 4. 공유 메모리 및 메시지 큐 삭제
    // SysV IPC는 명시적으로 삭제하지 않으면 리부팅 전까지 OS에 남아있음
    
    if( shm_delete() < 0 ) {
        printf("shm_delete() check failed (Process count mismatch or already deleted)\n");
    } else {
        printf("Shared Memory segments deleted.\n");
    }

    // 메시지 큐 삭제 (msg_func.c에 있는 함수)
    if (msg_all_delete() == TRUE) {
        printf("Message Queues deleted.\n");
    } else {
        printf("msg_all_delete() failed.\n");
    }

    exit(0);
}