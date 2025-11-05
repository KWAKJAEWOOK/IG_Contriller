/*
    조도 센서 프로세스
    1. 조도 센서로부터 조도 데이터 파싱
    2. 공유메모리에 업데이트
    3. 연결 해제 시 자동 재연결 시도
*/

/* 설정 
1. 전광판 조도 센서 연결 방법
    BH1750           라즈베리파이
    VCC     -        1번핀 (3.3V)
    GND     -        9번핀(GND)
    SCL     -        5번핀(SCL)
    SDA     -        3번핀(SDA)
    ADDR    -        6번핀(GND)
2. 라즈베리파이 설정
    sudo raspi-config   # Interface Options → I2C → Enable
    sudo apt-get update
    sudo apt-get install -y i2c-tools
    i2cdetect -y 1      # 0x23(또는 0x5C) 보이는지 확인
3. 프로그램 bh1750.c
   -. 0 ~ 65535 lux로 값(표출 : BH1750: 605.00 lux)
   -. 낮과 밤 값 체크 필요
   -. 컴파일 방법 : gcc -o bh1750 bh1750.c
*/
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#include "global/global.h"
#include "global/shm_type.h"
#include "global/msg_type.h"
#include "global/CommData.h"
#include "global/logger.h"

// 선택: ADDR=GND -> 0x23, ADDR=VCC -> 0x5C
#define BH1750_ADDR 0x23

// 명령어들
#define BH1750_POWER_ON      0x01
#define BH1750_RESET         0x07
#define BH1750_CONT_HIRES    0x10   // 1 lx resolution, typical 120ms

static int i2c_write_cmd(int fd, uint8_t cmd) {
    return write(fd, &cmd, 1) == 1 ? 0 : -1;
}

int main(void) {
    if (shm_all_open() == false) {
        printf("BH1750] shm_init() failed\n");
        exit(-1);
    }
    if (msg_all_open() == false){
        printf("BH1750] msg_all_open() failed\n");
        exit(-1);
    }

    proc_shm_ptr->pid[BH1750_Manager_PID] = getpid();
	proc_shm_ptr->status[BH1750_Manager_PID] = 'S';

    const char *i2c_dev = "/dev/i2c-1";
    int fd = open(i2c_dev, O_RDWR);
    if (fd < 0) {
        perror("open /dev/i2c-1");
        return 1;
    }
    if (ioctl(fd, I2C_SLAVE, BH1750_ADDR) < 0) {
        perror("ioctl I2C_SLAVE");
        close(fd);
        return 1;
    }

    system_set_ptr->brightness = -1.0;    // 값 초기화

    // 전원 ON → 리셋 → 연속 고해상도 모드 시작
    if (i2c_write_cmd(fd, BH1750_POWER_ON) < 0) { perror("POWER_ON"); return 1; }
    if (i2c_write_cmd(fd, BH1750_RESET)    < 0) { perror("RESET");    return 1; }
    if (i2c_write_cmd(fd, BH1750_CONT_HIRES) < 0) { perror("CONT_HIRES"); return 1; }

    // 첫 변환 대기 (데이터시트 typ 120ms)
    usleep(180 * 1000);

    while (1) {
        uint8_t buf[2];
        ssize_t n = read(fd, buf, 2);
        if (n != 2) {
            perror("read");
        } else {
            uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
            // 데이터시트: lux = raw / 1.2 (연속 고해상도 모드)
            double lux = raw / 1.2;
            printf("BH1750: %.2f lux\n", lux);
            system_set_ptr->brightness = lux; // 공유메모리에 조도값 업데이트
            fflush(stdout);
        }
        sleep(1); // 1s마다 읽기
    }
    if (shm_close() != 0)
	{
		printf("BH1750]  shm_close() failed\n");
		exit(-1);
	}
    close(fd);
    return 0;
}