#ifndef GLOBAL_H
#define GLOBAL_H

#include <stdint.h>
#include <stdbool.h>

/* ---- 공통 타입/상수 ---- */
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

/* 리눅스에는 없는 Windows형 typedef 보완 (또는 반대) */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>     /* BOOL, BYTE, WORD, DWORD 등 제공 */
  #include <basetsd.h>
  typedef SSIZE_T ssize_t;
  typedef long key_t;      /* System-V IPC 대체용 더미 */
#else
  typedef unsigned char BYTE;
  typedef int BOOL;
  #include <sys/types.h>   /* key_t 등 */
#endif

/* 프로젝트 기본 상수(원본 global.h가 없어서 합리적 기본값 지정) */
#ifndef MAX_HANDLE_INDEX
#define MAX_HANDLE_INDEX     64
#endif
#ifndef MAX_HOST_NAME_LENGTH
#define MAX_HOST_NAME_LENGTH 256
#endif
#ifndef CONNECT
#define CONNECT 1
#endif
#ifndef ACCEPT
#define ACCEPT 2
#endif

#endif /* GLOBAL_H */