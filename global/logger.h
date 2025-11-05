#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

// 로그 레벨 정의
#define LOG_LEVEL_INFO "INFO"
#define LOG_LEVEL_WARN "WARN"
#define LOG_LEVEL_ERROR "ERROR"
#define LOG_LEVEL_DEBUG "DEBUG"

// 로그 저장 관리를 위한 디스크 용량 스레스홀드 (MB))
#define DISK_SIZE_THRESHOLD 2000    // 2G

/**
 * @brief 로거를 초기화합니다. 프로그램 시작 시 한 번만 호출해야 합니다.
 * 로그 디렉토리를 확인/생성하고, 현재 로그 파일 이름을 설정합니다.
 * @param log_dir 로그 파일을 저장할 디렉토리 경로.
 * @param disk_usage_threshold 디스크 사용량 임계값. 이 값을 초과하면 오래된 로그 삭제를 시도합니다.
 * @return 성공 시 0, 실패 시 -1.
 */
int logger_init(const char* log_dir, int disk_usage_threshold);

/**
 * @brief 로거 리소스를 정리합니다. 프로그램 종료 시 한 번만 호출해야 합니다.
 */
void logger_cleanup();

/**
 * @brief 로그를 파일에 기록합니다. 날짜 변경 및 파일 크기에 따른 롤링을 지원합니다.
 * 이 함수는 스레드에 안전합니다.
 * @param level 로그 레벨 문자열 (예: LOG_LEVEL_INFO).
 * @param fmt 포맷 문자열 (printf와 동일).
 * @param ... 가변 인자.
 */
void logger_log(const char *level, const char *fmt, ...);


#endif // LOGGER_H