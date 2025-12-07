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
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (r == 0) return 0;
        total += (size_t)r;
    }
    return 1;
}

static int write_n(int fd, const void *buf, size_t n) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < n) {
        int w = send(fd, p + total, (int)(n - total), 0);
        if (w < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        total += (size_t)w;
    }
    return 1;
}

static void print_game_state(uint8_t word_len,
                             uint8_t num_wrong,
                             const char *data) {
    printf(">>>");
    for (int i = 0; i < word_len; i++) {
        printf("%c", data[i]);
        if (i < word_len - 1) {
            printf(" ");
        }
    }
    printf("\n");
    printf(">>>Incorrect Guesses:");
    printf(" ");
    for (int i = 0; i < num_wrong; i++) {
        printf("%c", data[word_len + i]);
        if (i < num_wrong - 1) {
            printf(" ");
        }
    }
    printf("\n");
    printf(">>>\n");
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
            uint8_t word_len = hdr[0];
            uint8_t num_wrong = hdr[1];
            int data_len = word_len + num_wrong;
            char *data = NULL;
            if (data_len > 0) {
                data = (char *)malloc((size_t)data_len);
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
        } else {
            uint8_t len = flag;
            char *msg = (char *)malloc((size_t)len + 1);
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
            if (strcmp(msg, "Game Over!") == 0) {
                *game_over = 1;
                free(msg);
                return 0;
            }
            free(msg);
        }
    }
    return 0;
}

static int check_overloaded_message(int sockfd) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    int rv = select(sockfd + 1, &rfds, NULL, NULL, &tv);
    if (rv > 0 && FD_ISSET(sockfd, &rfds)) {
        uint8_t flag;
        if (read_n(sockfd, &flag, 1) <= 0) {
            return 1;
        }
        if (flag > 0) {
            uint8_t len = flag;
            char *msg = (char *)malloc((size_t)len + 1);
            if (!msg) return 1;
            if (read_n(sockfd, msg, len) <= 0) {
                free(msg);
                return 1;
            }
            msg[len] = '\0';
            printf(">>>%s\n", msg);
            free(msg);
            return 1;
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
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    int sockfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
#ifdef _WIN32
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
        WSACleanup();
#else
        perror("socket");
#endif
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(server_ip);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "Invalid address: %s\n", server_ip);
        close(sockfd);
        WSACleanup();
        return 1;
    }
#else
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return 1;
    }
#endif

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        fprintf(stderr, "connect failed: %d\n", WSAGetLastError());
        close(sockfd);
        WSACleanup();
#else
        perror("connect");
        close(sockfd);
#endif
        return 1;
    }

    if (check_overloaded_message(sockfd)) {
        close(sockfd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

    char line[128];
    printf(">>>Ready to start game? (y/n): ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) {
        printf("\n");
        close(sockfd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }
    if (line[0] != 'y' && line[0] != 'Y') {
        close(sockfd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

    uint8_t start_len = 0;
    if (write_n(sockfd, &start_len, 1) <= 0) {
        close(sockfd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    int game_over = 0;

    while (!game_over) {
        int has_state = handle_packets(sockfd, &game_over);
        if (game_over || !has_state) break;

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
            guess_char = (char)tolower((unsigned char)guess_buf[0]);
            break;
        }
        if (game_over) break;

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
