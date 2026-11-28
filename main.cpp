//
// Created by lenovo on 2026/11/28.
//
#include<cstdio>
#include<cstdint>
#include<cstring>
#include<cstdlib>
#include<cerrno>
#include<sys/select.h>
#include<fcntl.h>
#include<unistd.h>
#include<termios.h>

static const unsigned char SYNC0=0xA5;
static const unsigned char SYNC1=0x5A;
static const unsigned char VER=0x03;
static const int MAX_FRAME=8+255;

static unsigned short crc16(const unsigned char* p,int n) {
    unsigned short crc=0xFFFF;
    for (int i=0;i<n;i++) {
        crc^=(unsigned short)p[i]<<8;
        for (int k=0;k<8;k++) {
            if (crc& 0x8000) {
                 crc=(unsigned short)((crc<<1)^0x1021);
            }else {
                crc=(unsigned short)(crc<<1);
            }

        }
    }
    return crc;
}