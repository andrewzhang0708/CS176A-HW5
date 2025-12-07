#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
    typedef HANDLE pthread_t;
    typedef CRITICAL_SECTION pthread_mutex_t;
    #define PTHREAD_MUTEX_INITIALIZER {0}
    #define pthread_mutex_lock(m)   EnterCriticalSection(m)
    #define pthread_mutex_unlock(m) LeaveCriticalSection(m)
    typedef int socklen_t;
#else
    #include <unistd.h>
    #include <errno.h>
    #include <pthread.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

#define MAX_CLIENTS   3
#define MAX_WORDS     32
#define MAX_WORD_LEN  16
#define MAX_WRONG     6

typedef struct {
    int sockfd;
} client_args_t;

static char words[MAX_WORDS][MAX_WORD_LEN];
static int num_words = 0;

static int current_clients = 0;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static int send_message_packet(int sockfd, const char *msg) {
    uint8_t len = (uint8_t)strlen(msg);
    if (len == 0) return 0;
    if (write_n(sockfd, &len, 1) <= 0) return -1;
    return write_n(sockfd, msg, len);
}

static int send_game_control_packet(int sockfd,
                                    const char *display, int word_len,
                                    const char *wrong_letters, int num_wrong) {
    uint8_t hdr[3];
    hdr[0] = 0;
    hdr[1] = (uint8_t)word_len;
    hdr[2] = (uint8_t)num_wrong;
    if (write_n(sockfd, hdr, 3) <= 0) return -1;
    if (write_n(sockfd, display, word_len) <= 0) return -1;
    if (num_wrong > 0 &&
        write_n(sockfd, wrong_letters, num_wrong) <= 0) return -1;
    return 1;
}

static void send_word_reveal(int sockfd, const char *word) {
    char msg[64];
    snprintf(msg, sizeof(msg), "The word was %s", word);
    send_message_packet(sockfd, msg);
}

#ifdef _WIN32
static DWORD WINAPI client_thread(LPVOID arg) {
#else
static void *client_thread(void *arg) {
#endif
    client_args_t *cargs = (client_args_t *)arg;
    int sockfd = cargs->sockfd;
    free(cargs);

    int idx;
    pthread_mutex_lock(&clients_mutex);
    idx = rand() % num_words;
    pthread_mutex_unlock(&clients_mutex);

    const char *word = words[idx];
    int word_len = (int)strlen(word);

    char display[MAX_WORD_LEN];
    char wrong_letters[MAX_WRONG];
    int num_wrong = 0;

    for (int i = 0; i < word_len; i++) {
        display[i] = '_';
    }

    uint8_t msg_len;
    if (read_n(sockfd, &msg_len, 1) <= 0) goto cleanup;
    if (msg_len != 0) goto cleanup;

    if (send_game_control_packet(sockfd, display, word_len,
                                 wrong_letters, num_wrong) <= 0)
        goto cleanup;

    int game_over = 0;

    while (!game_over) {
        uint8_t len;
        if (read_n(sockfd, &len, 1) <= 0) break;

        if (len == 0) {
            send_word_reveal(sockfd, word);
            send_message_packet(sockfd, "You Lose!");
            send_message_packet(sockfd, "Game Over!");
            break;
        }

        if (len != 1) {
            break;
        }

        char guess;
        if (read_n(sockfd, &guess, 1) <= 0) break;
        guess = (char)tolower((unsigned char)guess);

        int already_correct = 0;
        for (int i = 0; i < word_len; i++) {
            if (display[i] == guess) {
                already_correct = 1;
                break;
            }
        }

        int already_wrong = 0;
        for (int i = 0; i < num_wrong; i++) {
            if (wrong_letters[i] == guess) {
                already_wrong = 1;
                break;
            }
        }

        int found = 0;
        if (!already_correct) {
            for (int i = 0; i < word_len; i++) {
                if (word[i] == guess) {
                    display[i] = guess;
                    found = 1;
                }
            }
        }

        if (!found && !already_wrong && !already_correct) {
            if (num_wrong < MAX_WRONG) {
                wrong_letters[num_wrong++] = guess;
            }
        }

        int all_guessed = 1;
        for (int i = 0; i < word_len; i++) {
            if (display[i] == '_') {
                all_guessed = 0;
                break;
            }
        }

        if (all_guessed) {
            send_word_reveal(sockfd, word);
            send_message_packet(sockfd, "You Win!");
            send_message_packet(sockfd, "Game Over!");
            game_over = 1;
        } else if (num_wrong >= MAX_WRONG) {
            send_word_reveal(sockfd, word);
            send_message_packet(sockfd, "You Lose!");
            send_message_packet(sockfd, "Game Over!");
            game_over = 1;
        } else {
            if (send_game_control_packet(sockfd, display, word_len,
                                         wrong_letters, num_wrong) <= 0) {
                break;
            }
        }
    }

cleanup:
    close(sockfd);

    pthread_mutex_lock(&clients_mutex);
    current_clients--;
    pthread_mutex_unlock(&clients_mutex);

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void send_overloaded(int sockfd) {
    send_message_packet(sockfd, "server-overloaded");
    close(sockfd);
}

static void load_words(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror("fopen words file");
        exit(1);
    }
    char buf[256];
    while (num_words < MAX_WORDS && fgets(buf, sizeof(buf), fp)) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        if (buf[0] == '\0') continue;
        strncpy(words[num_words], buf, MAX_WORD_LEN - 1);
        words[num_words][MAX_WORD_LEN - 1] = '\0';
        num_words++;
    }
    fclose(fp);
    if (num_words == 0) {
        fprintf(stderr, "No words loaded from %s\n", filename);
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    int port;
    const char *words_file = "hangman_words.txt";

    if (argc == 2) {
        port = atoi(argv[1]);
    } else if (argc == 3) {
        port = atoi(argv[1]);
        words_file = argv[2];
    } else {
        fprintf(stderr, "Usage: %s <port> [words_file]\n", argv[0]);
        return 1;
    }

    load_words(words_file);
    srand(0);

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
    InitializeCriticalSection(&clients_mutex);
#endif

    int listenfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
#ifdef _WIN32
        fprintf(stderr, "socket failed: %d\n", WSAGetLastError());
#else
        perror("socket");
#endif
        return 1;
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        return 1;
    }

    if (listen(listenfd, 8) < 0) {
        perror("listen");
        close(listenfd);
        return 1;
    }

    printf("Hangman server listening on port %d\n", port);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int connfd = (int)accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);
        if (connfd < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            fprintf(stderr, "accept failed: %d\n", err);
#else
            if (errno == EINTR) continue;
            perror("accept");
#endif
            break;
        }

        pthread_mutex_lock(&clients_mutex);
        if (current_clients >= MAX_CLIENTS) {
            pthread_mutex_unlock(&clients_mutex);
            send_overloaded(connfd);
            continue;
        }
        current_clients++;
        pthread_mutex_unlock(&clients_mutex);

        client_args_t *cargs = (client_args_t *)malloc(sizeof(client_args_t));
        if (!cargs) {
            close(connfd);
            pthread_mutex_lock(&clients_mutex);
            current_clients--;
            pthread_mutex_unlock(&clients_mutex);
            continue;
        }
        cargs->sockfd = connfd;

        pthread_t tid;
#ifdef _WIN32
        tid = CreateThread(NULL, 0, client_thread, cargs, 0, NULL);
        if (tid == NULL) {
            fprintf(stderr, "CreateThread failed\n");
            close(connfd);
            pthread_mutex_lock(&clients_mutex);
            current_clients--;
            pthread_mutex_unlock(&clients_mutex);
            free(cargs);
            continue;
        }
        CloseHandle(tid);
#else
        pthread_create(&tid, NULL, client_thread, cargs);
        pthread_detach(tid);
#endif
    }

    close(listenfd);
#ifdef _WIN32
    DeleteCriticalSection(&clients_mutex);
    WSACleanup();
#endif
    return 0;
}
