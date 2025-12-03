#ifndef _SHM_TYPE_
#define _SHM_TYPE_

#include <sys/types.h>
#include <unistd.h>

#include "global.h"

// Process ID
enum {
	IG_SERVER_MANAGER_PID,
	LED_MANAGER_PID,
	M30_MANAGER_PID,
	CONNECTION_MANAGER_PID,
	BH1750_Manager_PID,
	TOTAL_PROCESS_NUMBER
};

#define MAX_PROCESS TOTAL_PROCESS_NUMBER

/* Process */
typedef struct {
	pid_t   pid[MAX_PROCESS];           /* Process PID                      	*/
	int     prio[MAX_PROCESS];          /* Process Priority */
	char	status[MAX_PROCESS];
	char    name[MAX_PROCESS][64];      /* Process or Thread Name           	*/
	char	version[MAX_PROCESS][12];    /* Process Version                  	*/
} SHM_PROC_DATA;

typedef struct {
	int				TimeTable_Cnt;
	unsigned int	Start_Time[24];
	unsigned int	End_Time[24];
	unsigned int 	Pedestrian_Speaker_Volume[24];
	unsigned int 	Pedestrian_Light_Brightness[24];
	unsigned int 	VMS_Brightness[5][24];
} timeTable;

// 메시지 큐 대체로 공유 메모리로 사용
enum {
	IG_SERVER_Q_PID,
	LED_MANAGER_Q_PID,
	M30_MANAGER_Q_PID,
	CONNECTION_MANAGER_Q_PID
};
#define BUF_SIZE 1024
typedef struct {
    volatile int head;
    volatile int tail;
    char buffer[BUF_SIZE];
} shm_ringbuf_t;

/*
	IG_Server_Manager에서 MESSAGEDATA를 기반으로 csv 파일과 대조 및 표출 우선순위 및 표출 내용을 계산,
	M30, LED 제어를 위한 상태를 담는 구조체
	M30_Manager, LED_Manager에선 이것을 참고하여 표출 패킷을 뿌림 (현재 표출 내용과 변경이 있는 장치에만)
*/
typedef struct {
    int MsgCount;	// 디버깅용, 원본 데이터의 MsgCount
	char Timestamp[32];

	int n_in_msg[3];	// 북쪽, 교차로 진입 방향 볼라드 그룹에 뿌릴 메시지 번호와, 상응하는 객체 속도, (PET_Threshold-PET) 값 (객체 속도나 PET 값이 없으면 -1로 저장함)
	int n_load_msg[3];	// 북쪽, 교차로 진입 방향 지주/가드레일 그룹에 뿌릴 메시지 번호와, 상응하는 객체 속도, PET 값
	int n_out_msg[3];	// 북쪽, 교차로 진출 방향 볼라드 그룹에 뿌릴 메시지 번호와, 상응하는 객체 속도, PET 값

	int e_in_msg[3];
	int e_load_msg[3];
	int e_out_msg[3];

	int s_in_msg[3];
	int s_load_msg[3];
	int s_out_msg[3];

	int w_in_msg[3];
	int w_load_msg[3];
	int w_out_msg[3];

	float brightness;	// 조도센서에서 가져온 밝기값
} VMS_COMMAND_DATA;

typedef struct {	// 소켓 통신 상태정보. true면 연결중, false면 연결 끊김
	bool ig_server_conn;

	bool led_conn;

	bool m30_n_in_comm[5];
	bool m30_n_load_comm[5];
	bool m30_n_out_comm[5];

	bool m30_e_in_comm[5];
	bool m30_e_load_comm[5];
	bool m30_e_out_comm[5];

	bool m30_s_in_comm[5];
	bool m30_s_load_comm[5];
	bool m30_s_out_comm[5];

	bool m30_w_in_comm[5];
	bool m30_w_load_comm[5];
	bool m30_w_out_comm[5];
} CONNECTION_STATUS;

typedef struct _system_set {
	int 	master_id;

	int				n_dir_code;				// 북쪽 도로의 방향 코드. (IG-Server에서 오는 CVIBDirCode 번호)
	int				e_dir_code;
	int				s_dir_code;
	int				w_dir_code;

	char			ig_server_ip[32];
	int				ig_server_port;
	char			led_ip[32];
	int				led_port;

	char			m30_n_in_ip[5][32];		// 북쪽 도로, 교차로 진입 방향 볼라드
	char			m30_n_load_ip[5][32];	// 북쪽 도로, 교차로 진입 방향 지주/가드레일
	char			m30_n_out_ip[5][32];	// 북쪽 도로, 교차로 진출 방향 볼라드

	char			m30_e_in_ip[5][32];		// 동
	char			m30_e_load_ip[5][32];
	char			m30_e_out_ip[5][32];

	char			m30_s_in_ip[5][32];		// 남
	char			m30_s_load_ip[5][32];
	char			m30_s_out_ip[5][32];

	char			m30_w_in_ip[5][32];		// 서
	char			m30_w_load_ip[5][32];
	char			m30_w_out_ip[5][32];

	int				m30_port;

	shm_ringbuf_t 	msg_IG_Server_Q;
	shm_ringbuf_t 	msg_LED_Q;
	shm_ringbuf_t 	msg_M30_Q;
	shm_ringbuf_t 	msg_Connection_manager_Q;

} SHM_SYSTEM_SET;

//================================ IG-Server에서 빋은 교통 상태정보 업데이트용 공유메시지 ================================	// 이거 다 가지고 있을 필요 없을듯
// typedef struct {	// 리스트 내부 객체 개수 업데이트용
// 	int ApproachTrafficInfo_count;	// ApproachTrafficInfoList 내부, 이번 프레임에 유효한 객체 개수
// 	// int HostObject_count;
// 	int RemoteObject_count;
// 	int HostObject_WayPoint_count;
// 	int RemoteObject_WayPoint_count;
// } LISTINFO;
// //====== LISTINFO는 별개 =====
// typedef struct {	// waypoint 구조체
// 	float lat;
// 	float lon;
// 	float elevation;
// 	float timeOffset;
// 	float speed;
// 	float heading;
// } WAYPOINT;
// typedef struct {	// Host Object 구조체
// 	char ObjectType[32];	// 객체 타입 (타입별 표기 TBD)
// 	char ObjectID[32];		// 객체 식별자
// 	bool IsDrivingIntentShared;	// 주행 의도 공유 여부 (0: 미공유, 1: 공유)
// 	int IGIntersectionIntent;	// IG 통신메시지 정의서의 DE_IGIntersectionIntent(교차로 주행의도 유형) 사용 // todo. 무슨 의미인지 잘 모르겠음
// 	int CVIBDirCode;	// 교차로 진입 방향정보 코드
// 	WAYPOINT WayPointList[20];	// 주행 예측 궤적 리스트
// } HOSTOBJECT;
// typedef struct {	// Remote Object 구조체
// 	char ObjectType[32];
// 	char ObjectID[32];
// 	bool IsDrivingIntentShared;
// 	int IGIntersectionIntent;
// 	// int CVIBDirCode;	// todo. 추가 예정
// 	WAYPOINT WayPointList[20];
// } REMOTEOBJECT;
// typedef struct {	// ConflictPos 구조체
// 	float lat;
// 	float lon;
// 	// float elevation;	// 필요없을듯
// } CONFLICTPOS;
// typedef struct {	// ApproachTrafficInfo 구조체
// 	CONFLICTPOS ConflictPos;
// 	float PET;
// 	float PET_Threshold;
// 	HOSTOBJECT HostObject[4];
// 	REMOTEOBJECT RemoteObject[4];
// } APPROACHTRAFFICINFO;
// typedef struct {	// 전체 메시지 프레임
// 	int MsgCount;
// 	char Timestamp[32];
// 	APPROACHTRAFFICINFO ApproachTrafficInfoList[4];
// } MESSAGEDATA;
//=========================================================================================
typedef struct {	// 간소화시킨 IG-Server 수신 메시지
	int MsgCount;
	char Timestamp[32];
	int Num_Of_ApproachTrafficInfo;	// 이번 프레임에 유효한 ApproachTrafficInfo 개수
	struct {
		struct {	// ConflictPos
			float lat;
			float lon;
			// float alt;	// 안씀
		} ConflictPos;	// 없으면 다 -1
		float PET;	// 없으면 다 -1
		float PET_Threshold;
		struct {	// HostObject
			char ObjectType[32];
			char ObjectID[32];
			// bool IsDrivingIntentShared;
			// int IGIntersectionIntent;
			int CVIBDirCode;
			int Num_Of_HO_WayPoint;	// 유효한 HO_WayPoint 개수
			struct {
				float lat;
				float lon;
				// float alt;	// 안씀
				// float timeOffset;	// 안씀
				float speed;
				// float heading;	// 사실상 안들어옴
			} WayPoint[23];	// 최대 23개 제한 (웨이즈원)
		} HostObject;
		struct {	// RemoteObject
			char ObjectType[32];
			char ObjectID[32];
			// bool IsDrivingIntentShared;
			// int IGIntersectionIntent;
			// int CVIBDirCode;	// todo. 웨이즈원에 추가 요청 중
			int Num_Of_RO_WayPoint;	// 유효한 RO_WayPoint 개수
			struct {
				float lat;
				float lon;
				// float alt;	// 안씀
				// float timeOffset;	// 안씀
				float speed;
				// float heading;	// 사실상 안들어옴
			} WayPoint[23];	// 최대 23개 제한 (웨이즈원)
		} RemoteObject;	// 각 ApproachTrafficInfo당 최대 1개, 있을수도 없을수도 (ConflictPos나 PET가 있으면 있음)
	} ApproachTrafficInfo[4];
} MESSAGEDATA;
// Shared Memory -------------------------------------------------
#define SHM_MAX_COUNT			2
#define SHM_KEY_PROCESS			10000
#define SHM_KEY_SYSTEM_SET		10001

#define SHM_KEY_MESSAGEDATA		10002
// #define SHM_KEY_LISTINFO		10003

enum {
	SHMID_PROCESS_DATA = 0,
	SHMID_SYSTEM_SET,
	SHMID_COMMAND_SET,
	SHMID_CONNECTION_SET,
	SHMID_MESSAGEDATA,
	// SHMID_LISTINFO
};

int  	shm_create(key_t shm_key, long shm_size);
int 	shm_open(int shmid, int type );
int  	shm_close();
int  	shm_delete();

int 	shm_all_create();
int		shm_all_open();

// Message Queue -----------------------------------------------
int 	mp_All_create();

// End of Message Queue ----------------------------------------
extern SHM_PROC_DATA				*proc_shm_ptr;
extern SHM_SYSTEM_SET     			*system_set_ptr;

extern VMS_COMMAND_DATA				*vms_command_ptr;
extern CONNECTION_STATUS			*connection_status_ptr;
extern MESSAGEDATA					*message_data_ptr;
// extern LISTINFO						*list_info_ptr;

extern int 	 			shmid_proc;
extern int				shmid_system_set;

#endif /* #ifndef _SHM_TYPE_ */