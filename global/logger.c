// logger.c (전체 교체)
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

typedef struct {
    char log_dir[256];
    char current_logfile_path[512];
    long long size_threshold_bytes; // 임계값을 바이트 단위로 저장
    pthread_mutex_t lock;
} LoggerState;

static LoggerState g_logger;

// 로그 디렉토리의 전체 크기를 계산하는 함수 (바이트 단위)
static long long get_directory_size(const char* path) {
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
            if (S_ISDIR(st.st_mode)) {
                total_size += get_directory_size(full_path); // 하위 디렉토리 크기 재귀 호출
            } else {
                total_size += st.st_size;
            }
        }
    }
    closedir(d);
    return total_size;
}

// 가장 오래된 로그 파일(.log)을 찾아 삭제하는 함수
static void remove_oldest_log_file() {
    DIR *d = opendir(g_logger.log_dir);
    if (!d) return;

    struct dirent *dir;
    char oldest_file[256] = "";
    time_t oldest_time = 0;

    // 모든 하위 디렉토리를 포함하여 탐색
    char subdir_path[512];
    snprintf(subdir_path, sizeof(subdir_path), "%s", g_logger.log_dir);
    
    DIR *sd = opendir(subdir_path);
    if(sd) {
        while ((dir = readdir(sd)) != NULL) {
             if (strstr(dir->d_name, ".log")) {
                char full_path[1024];
                snprintf(full_path, sizeof(full_path), "%s/%s", subdir_path, dir->d_name);

                struct stat st;
                if (stat(full_path, &st) == 0) {
                    if (oldest_time == 0 || st.st_mtime < oldest_time) {
                        oldest_time = st.st_mtime;
                        strcpy(oldest_file, full_path);
                    }
                }
            }
        }
        closedir(sd);
    }
    closedir(d);

    if (strcmp(oldest_file, "") != 0) {
        if (remove(oldest_file) == 0) {
            logger_log(LOG_LEVEL_WARN, "Log directory size exceeded threshold. Removed oldest log: %s", oldest_file);
        } else {
            logger_log(LOG_LEVEL_ERROR, "Failed to remove oldest log: %s", oldest_file);
        }
    }
}


int logger_init(const char* log_dir, int disk_size_threshold_mb) {
    if (pthread_mutex_init(&g_logger.lock, NULL) != 0) {
        perror("logger: failed to initialize mutex");
        return -1;
    }
    snprintf(g_logger.log_dir, sizeof(g_logger.log_dir), "%s", log_dir);
    // MB를 바이트로 변환하여 저장
    g_logger.size_threshold_bytes = (long long)disk_size_threshold_mb * 1024 * 1024;
    
    struct stat st = {0};
    if (stat(g_logger.log_dir, &st) == -1) {
        mkdir(g_logger.log_dir, 0755);
    }
    
    // 초기 로그 파일 이름 설정 (실제 파일 생성은 logger_log에서)
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char date_hour_str[14];
    snprintf(date_hour_str, sizeof(date_hour_str), "%04d-%02d-%02d-%02d", 
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour);
    snprintf(g_logger.current_logfile_path, sizeof(g_logger.current_logfile_path), 
             "%s/%s.log", g_logger.log_dir, date_hour_str);

    printf("Logger initialized. Log file will be: %s\n", g_logger.current_logfile_path);
    return 0;
}

void logger_cleanup() {
    pthread_mutex_destroy(&g_logger.lock);
}

void logger_log(const char *level, const char *fmt, ...) {
    pthread_mutex_lock(&g_logger.lock);

    // 1. 현재 로그 파일 경로 업데이트 (시간 변경 감지)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char date_hour_str[14];
    snprintf(date_hour_str, sizeof(date_hour_str), "%04d-%02d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour);
    snprintf(g_logger.current_logfile_path, sizeof(g_logger.current_logfile_path),
             "%s/%s.log", g_logger.log_dir, date_hour_str);

    // 2. 로그 폴더 용량 확인 및 오래된 파일 삭제 (매번 로그를 쓸 때마다 확인)
    long long current_size = get_directory_size(g_logger.log_dir);
    while (current_size > g_logger.size_threshold_bytes) {
        remove_oldest_log_file();
        current_size = get_directory_size(g_logger.log_dir); // 삭제 후 크기 다시 확인
    }

    // 3. 파일에 로그 기록
    FILE *fp = fopen(g_logger.current_logfile_path, "a");
    if (!fp) {
        perror("logger: failed to open log file");
        pthread_mutex_unlock(&g_logger.lock);
        return;
    }
    int millisec = tv.tv_usec / 1000;
    fprintf(fp, "[%s][%02d:%02d:%02d.%03d] ",
            level, t->tm_hour, t->tm_min, t->tm_sec, millisec);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fclose(fp);
    
    pthread_mutex_unlock(&g_logger.lock);
}