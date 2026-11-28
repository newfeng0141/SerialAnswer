// ============================================================
// 星门织仪 · 最小串口工具（单文件版，手敲用）
//
// 编译：g++ -std=c++17 -Wall -Wextra answer.cpp -o answer
// 运行：./answer /dev/pts/3        ← 判题机窗口里「通讯铭录」给的路径
//
// 用法（十六进制，空格分隔；程序自动补 A5 5A、LEN 和 CRC）：
//   01 01                          读铭牌
//   20 02 00 00 70 42              定向到 60.0°（float 小端）
//   02 03                          取样
//   30 04 02 00 01 00 00 70 42 80 03   底片入账：样本号(2) 通道(1) 角(4) 光强(2)
//   03 05                          查底账
//   40 06                          开星门（三路留档都 ≥ 门限后再发！）
//   q                              退出
//
// 本工具只做三件事：补 CRC 组帧、收发帧、把回条拆成人能读的字段。
// 差角自己算、往哪边试自己定、结果自己记——程序不替你做判断。
// ============================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

// ---------- 常量（手册「帧格式」） ----------
static const unsigned char SYNC0 = 0xA5;
static const unsigned char SYNC1 = 0x5A;
static const unsigned char VER   = 0x03;
static const int MAX_FRAME = 8 + 255;   // 同步2 + VER1 + TYPE1 + SEQ1 + LEN1 + DATA + CRC2

// ---------- 1. CRC-16/CCITT-FALSE ----------
// init=0xFFFF，poly=0x1021，不反射，xorout=0
// 范围：VER、TYPE、SEQ、LEN、DATA —— 不含同步头 A5 5A
// 自测：crc16((const unsigned char*)"123456789", 9) == 0x29B1
static unsigned short crc16(const unsigned char* p, int n) {
    unsigned short crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= (unsigned short)p[i] << 8;
        for (int k = 0; k < 8; k++) {
            if (crc & 0x8000)
                crc = (unsigned short)((crc << 1) ^ 0x1021);
            else
                crc = (unsigned short)(crc << 1);
        }
    }
    return crc;
}

// ---------- 2. 组帧 ----------
// 返回整帧字节数；out 至少要 MAX_FRAME 字节
static int build_frame(unsigned char type, unsigned char seq,
                       const unsigned char* data, int dlen,
                       unsigned char* out) {
    if (dlen < 0 || dlen > 255) return 0;          // LEN 只有一个字节，超了就拒
    out[0] = SYNC0;
    out[1] = SYNC1;
    out[2] = VER;
    out[3] = type;
    out[4] = seq;
    out[5] = (unsigned char)dlen;
    for (int i = 0; i < dlen; i++) out[6 + i] = data[i];
    unsigned short c = crc16(out + 2, 4 + dlen);   // 从 VER 算到 DATA 末尾
    out[6 + dlen] = (unsigned char)(c & 0xFF);     // 低字节在前
    out[7 + dlen] = (unsigned char)(c >> 8);       // 高字节在后
    return 8 + dlen;
}

// ---------- 3. 打开串口 ----------
static int open_serial(const char* path) {
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return -1; }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { perror("tcgetattr"); close(fd); return -1; }

    cfmakeraw(&tio);                  // 关回显、关行缓冲：否则要按回车才收到
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);              // 本地连接、允许接收
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);    // 无校验、1 停止位
    tio.c_cflag |= CS8;                           // 8 数据位
    tio.c_cc[VMIN]  = 0;                          // 读不等待（配合 select）
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) != 0) { perror("tcsetattr"); close(fd); return -1; }
    return fd;
}

// ---------- 4. 读一个字节（带超时） ----------
// 返回 0..255；超时或出错返回 -1
static int read_byte(int fd, int timeout_ms) {
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int r = select(fd + 1, &set, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;     // 被信号打断，重来
        if (r <= 0) return -1;                     // 超时

        unsigned char b = 0;
        ssize_t n = read(fd, &b, 1);
        if (n == 1) return (int)b;
        if (n < 0 && errno == EINTR) continue;
        return -1;                                 // 关键：别把"没读到"当 0x00 返回
    }
}

// ---------- 5. 读一整帧 ----------
// 返回帧长（含同步头与 CRC）；-1 超时；-2 CRC 不符
static int read_frame(int fd, unsigned char* buf, int cap, int timeout_ms) {
    // 5.1 逐字节滑动，遇到 A5 5A 才认帧头（字节流没有消息边界）
    int prev = -1;
    for (;;) {
        int b = read_byte(fd, timeout_ms);
        if (b < 0) return -1;
        if (prev == SYNC0 && b == SYNC1) break;
        prev = b;
    }

    // 5.2 同步头已吃进，接着读 VER TYPE SEQ LEN
    buf[0] = SYNC0;
    buf[1] = SYNC1;
    for (int i = 2; i < 6; i++) {
        int b = read_byte(fd, 200);
        if (b < 0) return -1;
        buf[i] = (unsigned char)b;
    }

    // 5.3 按 LEN 读 DATA + CRC
    int len = buf[5];
    int total = 8 + len;
    if (total > cap) return -1;
    for (int i = 0; i < len + 2; i++) {
        int b = read_byte(fd, 200);
        if (b < 0) return -1;
        buf[6 + i] = (unsigned char)b;
    }

    // 5.4 校验 CRC：范围是 VER..DATA
    unsigned short want = crc16(buf + 2, 4 + len);
    unsigned short got  = (unsigned short)(buf[6 + len] | (buf[7 + len] << 8));
    if (want != got) {
        printf("  [CRC 不符] 期望 %04X，收到 %04X\n", (unsigned)want, (unsigned)got);
        return -2;
    }
    return total;
}

// ---------- 6. 从载荷取字段（小端） ----------
static unsigned short u16(const unsigned char* p, int i) {
    return (unsigned short)(p[i] | (p[i + 1] << 8));
}

static float f32(const unsigned char* p, int i) {
    unsigned int bits = (unsigned int)p[i]
                      | ((unsigned int)p[i + 1] << 8)
                      | ((unsigned int)p[i + 2] << 16)
                      | ((unsigned int)p[i + 3] << 24);
    float v;
    memcpy(&v, &bits, 4);        // 别用 *(float*)&bits：类型双关是未定义行为
    return v;
}

// ---------- 7. 打印 ----------
static void hexdump(const char* tag, const unsigned char* p, int n) {
    printf("%s", tag);
    for (int i = 0; i < n; i++) printf("%02X ", (unsigned)p[i]);
    printf("\n");
}

static void show_reply(const unsigned char* f, int n) {
    if (n < 8) { printf("  回条太短\n"); return; }
    int len = f[5];
    const unsigned char* p = f + 6;              // 载荷起点
    printf("  TYPE=%02X SEQ=%d LEN=%d\n", (unsigned)f[3], f[4], len);

    switch (f[3]) {
    case 0x81:                                   // 铭牌：20 字节
        if (len < 20) { printf("  载荷长度不足\n"); break; }
        printf("  铭牌: 星图=%d 粗角=%.1f/%.1f/%.1f 轴速=%.2f 门限=%d\n",
               u16(p, 0), f32(p, 2), f32(p, 6), f32(p, 10), f32(p, 14), u16(p, 18));
        break;

    case 0xA0:                                   // 定向：9 字节
        if (len < 9) { printf("  载荷长度不足\n"); break; }
        printf("  定向: 实测角=%.1f 目标角=%.1f %s\n",
               f32(p, 0), f32(p, 4), p[8] ? "转动中" : "已停稳");
        break;

    case 0x82:                                   // 取样：18 字节
        if (len < 18) { printf("  载荷长度不足\n"); break; }
        printf("  取样: 样本号=%d 实测角=%.1f 目标角=%.1f 光强=%d/%d/%d %s 刻度=%d\n",
               u16(p, 0), f32(p, 2), f32(p, 6),
               u16(p, 10), u16(p, 12), u16(p, 14),
               p[16] ? "转动中" : "已停稳", p[17]);
        break;

    case 0x83:                                   // 底账：14 字节
        if (len < 14) { printf("  载荷长度不足\n"); break; }
        printf("  底账: 扫描=%d/%d/%d 留档=%d/%d/%d 刻度=%d 星门=%s\n",
               u16(p, 0), u16(p, 2), u16(p, 4),
               u16(p, 6), u16(p, 8), u16(p, 10), p[12],
               p[13] ? "已开" : "未开");
        break;

    case 0x80:                                   // 通用应答：3 字节
        if (len < 3) { printf("  载荷长度不足\n"); break; }
        printf("  应答: 原接口=%02X 状态码=%02X 刻度=%d\n",
               (unsigned)p[0], (unsigned)p[1], p[2]);
        break;

    default:
        printf("  未知接口类型\n");
    }
}

// ---------- 8. 主循环 ----------
int main(int argc, char** argv) {
    if (argc != 2) { printf("用法: %s /dev/pts/3\n", argv[0]); return 1; }

    int fd = open_serial(argv[1]);
    if (fd < 0) return 1;

    setvbuf(stdout, NULL, _IONBF, 0);   // 不缓冲：提示和回条的先后顺序才对
    printf("已连 %s（115200 8N1）。输入 TYPE SEQ [DATA...]，q 退出。\n", argv[1]);

    char line[512];
    for (;;) {
        printf("> ");
        if (!fgets(line, sizeof(line), stdin)) break;
        if (line[0] == 'q' || line[0] == 'Q') break;

        // 8.1 把一行十六进制拆开：第 1 个 TYPE、第 2 个 SEQ、其余 DATA
        unsigned char data[256];
        unsigned char type = 0, seq = 0;
        int dlen = 0, cnt = 0;
        char* p = line;
        while (*p) {
            char* end = NULL;
            long v = strtol(p, &end, 16);
            if (end == p) { p++; continue; }        // 跳过空格、换行
            if (cnt == 0)      type = (unsigned char)v;
            else if (cnt == 1) seq  = (unsigned char)v;
            else if (dlen < 256) data[dlen++] = (unsigned char)v;
            cnt++;
            p = end;
        }
        if (cnt < 2) { printf("  至少输入 TYPE 和 SEQ\n"); continue; }

        // 8.2 组帧 → 发 → 收 → 拆
        unsigned char frame[MAX_FRAME];
        int n = build_frame(type, seq, data, dlen, frame);
        if (n == 0) { printf("  载荷太长（>255）\n"); continue; }

        hexdump("TX: ", frame, n);
        ssize_t w = write(fd, frame, (size_t)n);
        if (w != n) { perror("write"); continue; }

        unsigned char reply[MAX_FRAME];
        int m = read_frame(fd, reply, sizeof(reply), 1500);   // 等 1.5 秒
        if (m == -1) { printf("  回条超时\n"); continue; }
        if (m < 0) continue;                                   // CRC 不符已在里面打印

        hexdump("RX: ", reply, m);
        show_reply(reply, m);
    }

    close(fd);
    return 0;
}
