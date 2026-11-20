#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "maix_drv.h"

#define CLEAR   "\033[2J\033[H"
#define HIDE_C  "\033[?25l"
#define SHOW_C  "\033[?25h"
#define BOLD    "\033[1m"
#define GREEN   "\033[32m"
#define CYAN    "\033[36m"
#define YELLOW  "\033[33m"
#define RED     "\033[31m"
#define MAGENTA "\033[35m"
#define RESET   "\033[0m"

static volatile sig_atomic_t g_running = 1;
static struct termios g_orig_term;

static void restore_term(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_term);
    printf(SHOW_C RESET "\n");
}

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static void set_nonblock_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_term);
    atexit(restore_term);
    raw = g_orig_term;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(void) {
    int fd = open("/dev/maix0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/maix0");
        return 1;
    }

    signal(SIGINT, on_sigint);
    set_nonblock_raw();
    printf(HIDE_C CLEAR);

    /* Ensure unlimited rate on startup */
    uint32_t rate = 0;
    ioctl(fd, MAIX_IOC_SET_RATE, &rate);

    #define WAVE_W 56
    int wave[WAVE_W] = {0};
    int mode = 1; /* 1 = Burst Stream, 2 = Throttled 12 pps, 3 = Firewall Attack Test */
    uint8_t payload[48];
    memset(payload, 0x5A, sizeof(payload));

    uint64_t last_tx = 0;
    uint64_t last_sample_ns = now_ns();
    double live_kbps = 0.0;
    uint64_t live_pps = 0;
    char status_banner[128] = GREEN "ACTIVE: High-Speed USB Bulk URB Stream (Watch TXD LED on Board!)" RESET;

    while (g_running) {
        /* Read live hotkey */
        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            if (ch == 'q' || ch == 'Q') break;
            if (ch == '1') {
                mode = 1;
                rate = 0;
                ioctl(fd, MAIX_IOC_SET_RATE, &rate);
                snprintf(status_banner, sizeof(status_banner),
                         GREEN "MODE [1]: UNLIMITED BURST — Full USB URB Wire Speed (TXD Solid)" RESET);
            } else if (ch == '2') {
                mode = 2;
                rate = 12;
                ioctl(fd, MAIX_IOC_SET_RATE, &rate);
                snprintf(status_banner, sizeof(status_banner),
                         YELLOW "MODE [2]: RING-0 RATE LIMITER (12 pkt/s) — TXD Pulses Slowly!" RESET);
            } else if (ch == '3') {
                mode = 3;
                snprintf(status_banner, sizeof(status_banner),
                         RED "MODE [3]: FIREWALL ATTACK — Injecting Forged Checksums (-EBADMSG)" RESET);
            } else if (ch == '4') {
                snprintf(status_banner, sizeof(status_banner),
                         MAGENTA "MODE [4]: 4-PROCESS CONCURRENCY STORM — Testing io_mutex & rx_lock..." RESET);
                for (int p = 0; p < 4; p++) {
                    if (fork() == 0) {
                        int cfd = open("/dev/maix0", O_RDWR);
                        if (cfd >= 0) {
                            for (int k = 0; k < 40; k++) {
                                ssize_t wr = write(cfd, payload, 32);
                                (void)wr;
                            }
                            close(cfd);
                        }
                        _exit(0);
                    }
                }
                for (int p = 0; p < 4; p++) wait(NULL);
            }
        }

        /* Drive USB bus according to active mode */
        if (mode == 1 || mode == 2) {
            for (int b = 0; b < 6; b++) {
                ssize_t wr = write(fd, payload, sizeof(payload));
                (void)wr;
            }
        } else if (mode == 3) {
            /* Send 1 valid frame + 3 forged bad-checksum attack frames */
            uint8_t good[8] = {0xAA, 0x55, MAIX_CMD_LED, 3, 1, 0, 1, 0};
            good[7] = good[2] ^ good[3] ^ good[4] ^ good[5] ^ good[6];
            ioctl(fd, MAIX_IOC_RAW_FRAME, good);

            uint8_t bad[8]  = {0xDE, 0xAD, 0xFF, 3, 9, 9, 9, 0x13};
            ioctl(fd, MAIX_IOC_RAW_FRAME, bad);
            ioctl(fd, MAIX_IOC_RAW_FRAME, bad);
        }

        /* Query Ring-0 Telemetry via IOCTL */
        struct maix_hw_stats st = {0};
        ioctl(fd, MAIX_IOC_GET_STATS, &st);

        uint64_t t_now = now_ns();
        double dt_s = (t_now - last_sample_ns) / 1e9;
        if (dt_s >= 0.12) {
            uint64_t d_pkt = st.tx_packets - last_tx;
            live_pps = (uint64_t)(d_pkt / dt_s);
            live_kbps = (d_pkt * 52.0) / 1024.0 / dt_s;
            last_tx = st.tx_packets;
            last_sample_ns = t_now;

            int level = (int)(live_pps / 25);
            if (level > 8) level = 8;
            if (d_pkt > 0 && level == 0) level = 1;
            memmove(wave, wave + 1, (WAVE_W - 1) * sizeof(int));
            wave[WAVE_W - 1] = level;
        }

        /* Render Live Terminal Oscilloscope */
        printf("\033[H");
        printf(BOLD CYAN "╔══════════════════════════════════════════════════════════════════════╗\n");
        printf("║   RING-0 USB KERNEL OSCILLOSCOPE & PACKET FIREWALL (/dev/maix0)      ║\n");
        printf("╚══════════════════════════════════════════════════════════════════════╝\n" RESET);
        printf(" %s\033[K\n\n", status_banner);

        printf(BOLD " [LIVE USB BUS THROUGHPUT OSCILLOSCOPE]   Rate: %4llu pkt/s  |  %5.1f KB/s\n" RESET,
               (unsigned long long)live_pps, live_kbps);
        for (int row = 8; row >= 1; row--) {
            printf("  %3d |", row * 25);
            for (int col = 0; col < WAVE_W; col++) {
                if (wave[col] >= row) {
                    if (row >= 7) printf(RED "█" RESET);
                    else if (row >= 4) printf(YELLOW "█" RESET);
                    else printf(GREEN "█" RESET);
                } else {
                    printf(" ");
                }
            }
            printf("|\n");
        }
        printf("    0 +--------------------------------------------------------+\n\n");

        printf(BOLD " [RING-0 KERNEL TELEMETRY (/proc/maix_stats & IOCTL)]\n" RESET);
        printf("  • Hardware Link   : %s (VID:0403 PID:6001 | Major 180)\n",
               st.connected ? GREEN "ONLINE (USB Bound)" RESET : RED "DISCONNECTED" RESET);
        printf("  • Valid USB URBs  : " GREEN BOLD "%-10llu" RESET "   • USB URB Errors : " GREEN "%-8llu\n" RESET,
               (unsigned long long)st.tx_packets, (unsigned long long)st.urb_errors);
        printf("  • Firewall Drops  : " RED BOLD "%-10llu" RESET "   • Rate Throttled : " YELLOW BOLD "%-8llu\n" RESET,
               (unsigned long long)st.fw_dropped, (unsigned long long)st.rate_throttled);
        printf("  • Kernel Rate Cap : " CYAN "%u pkt/s" RESET " %s\n\n",
               st.rate_limit_pps, st.rate_limit_pps == 0 ? "(Unlimited)" : "(Enforced in Ring 0)\033[K");

        printf(BOLD " [INTERACTIVE KERNEL CONTROLS]\n" RESET);
        printf("  " BOLD "[1]" RESET " Burst Wire Speed    "
               BOLD "[2]" RESET " Throttle (12 pps)    "
               BOLD "[3]" RESET " Firewall Attack    "
               BOLD "[4]" RESET " 4-Proc Stress    "
               BOLD "[Q]" RESET " Quit\n");
        fflush(stdout);
        usleep(35000);
    }

    rate = 0;
    ioctl(fd, MAIX_IOC_SET_RATE, &rate);
    close(fd);
    return 0;
}
