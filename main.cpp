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
