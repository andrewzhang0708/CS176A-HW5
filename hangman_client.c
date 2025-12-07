#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#define close closesocket
#else
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#endif

static int read_n(int fd, void *buf, size_t n) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < n) {
        int r = recv(fd, p + total, (int)(n - total), 0);
        if (r < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#else
            if (WSAGetLastError() == WSAEINTR) continue;
#endif
            return -1;
        }
        if (r == 0) return 0;
        total += r;
    }
    return 1;
}

static int write_n(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < n) {
        int w = send(fd, p + total, (int)(n - total), 0);
        if (w < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#else
            if (WSAGetLastError() == WSAEINTR) continue;
#endif
            return -1;
        }
        total += w;
    }
    return 1;
}

static void print_game_state(uint8_t word_len, uint8_t num_wrong, const char *data) {
    printf(">>>");
    for (int i = 0; i < word_len; i++) {
        printf("%c", data[i]);
        if (i < word_len - 1) printf(" ");
    }
    printf("\n>>>Incorrect Guesses: ");
    for (int i = 0; i < num_wrong; i++) {
        printf("%c", data[word_len + i]);
        if (i < num_wrong - 1) printf(" ");
    }
    printf("\n>>>\n");
}

static int handle_packets(int sockfd, int *game_over) {
    while (!*game_over) {
        uint8_t flag;
        int r = read_n(sockfd, &flag, 1);
        if (r <= 0) {
            *game_over = 1;
            return 0;
        }

        if (flag == 0) {
            uint8_t hdr[2];
            if (read_n(sockfd, hdr, 2) <= 0) {
                *game_over = 1;
                return 0;
            }
            uint8_t word_len = hdr[0], num_wrong = hdr[1];
            int data_len = word_len + num_wrong;

            char *data = NULL;
            if (data_len > 0) {
                data = malloc(data_len);
                if (!data) {
                    *game_over = 1;
                    return 0;
                }
                if (read_n(sockfd, data, data_len) <= 0) {
                    free(data);
                    *game_over = 1;
                    return 0;
                }
            }

            print_game_state(word_len, num_wrong, data);
            free(data);
            return 1;
        }

        uint8_t len = flag;
        char *msg = malloc(len + 1);
        if (!msg) {
            *game_over = 1;
            return 0;
        }
        if (read_n(sockfd, msg, len) <= 0) {
            free(msg);
            *game_over = 1;
            return 0;
        }
        msg[len] = '\0';

        printf(">>>%s\n", msg);

        if (strcmp(msg, "server-overloaded") == 0 ||
            strcmp(msg, "Game Over!") == 0) {
            free(msg);
            *game_over = 1;
            return 0;
        }

        free(msg);
    }
    return 0;
}

static int check_overloaded_message(int sockfd) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(sockfd, &set);
    struct timeval tv = {1, 0};
    int rv = select(sockfd + 1, &set, NULL, NULL, &tv);

    if (rv > 0 && FD_ISSET(sockfd, &set)) {
        uint8_t flag;
        if (read_n(sockfd, &flag, 1) <= 0)
            return 1;
        if (flag > 0) {
            uint8_t len = flag;
            char *msg = malloc(len + 1);
            if (!msg) return 1;
            if (read_n(sockfd, msg, len) <= 0) {
                free(msg);
                return 1;
            }
            msg[len] = '\0';
            printf(">>>%s\n", msg);

            int overloaded = strcmp(msg, "server-overloaded") == 0;
            free(msg);
            return overloaded;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2,2), &w);
#endif

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return 1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    if (check_overloaded_message(sockfd)) {
        close(sockfd);
        return 0;
    }

    char line[128];
    printf(">>>Ready to start game? (y/n): ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) {
        printf("\n");
        close(sockfd);
        return 0;
    }
    if (line[0] != 'y' && line[0] != 'Y') {
        close(sockfd);
        return 0;
    }

    uint8_t zero = 0;
    if (write_n(sockfd, &zero, 1) <= 0) {
        close(sockfd);
        return 1;
    }

    int game_over = 0;

    while (!game_over) {
        int has_state = handle_packets(sockfd, &game_over);
        if (game_over || !has_state)
            break;

        char guess_buf[128];
        char guess_char = 0;

        while (1) {
            printf(">>>Letter to guess: ");
            fflush(stdout);

            if (!fgets(guess_buf, sizeof(guess_buf), stdin)) {
                printf("\n");
                game_over = 1;
                break;
            }

            char *nl = strchr(guess_buf, '\n');
            if (nl) *nl = '\0';

            if (strlen(guess_buf) != 1 || !isalpha((unsigned char)guess_buf[0])) {
                printf(">>>Error! Please guess one letter.\n");
                continue;
            }

            guess_char = (char)tolower(guess_buf[0]);
            break;
        }

        if (game_over)
            break;

        uint8_t len = 1;
        if (write_n(sockfd, &len, 1) <= 0) break;
        if (write_n(sockfd, &guess_char, 1) <= 0) break;
    }

    close(sockfd);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
