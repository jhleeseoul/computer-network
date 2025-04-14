#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#include "macro.h"
#define MAX_VAL  (MAX_HDR)
#define MAX_URL 1024

static const char* g_rootDir = "./";
const char *errMessage400 = "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n";
const char *errMessage404 = "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
const char *errMessage500 = "HTTP/1.0 500 Internal Server Error\r\nConnection: close\r\n\r\n";

int is_connection_close(const char *buf) {
    const char *conn = strstr(buf, "Connection:");
    if (!conn) return 0;
    while (*conn && *conn != '\r' && *conn != '\n') {
        if (strncasecmp(conn, "close", 5) == 0) return 1;
        conn++;
    }
    return 0;
}

int is_connection_keep_alive(const char *buf) {
    const char *conn = strstr(buf, "Connection:");
    if (!conn) return 0; 
    while (*conn && *conn != '\r' && *conn != '\n') {
        if (strncasecmp(conn, "keep-alive", 10) == 0) return 1;
        conn++;
    }
    return 0;
}

void handle_request(int client_fd) {
    char buffer[MAX_HDR + 1];
    int total_received = 0;

    while (1) {
        memset(buffer, 0, sizeof(buffer));
        total_received = 0;

        // 누적 read() - 헤더 끝 찾을 때까지
        while (total_received < MAX_HDR) {
            int r = read(client_fd, buffer + total_received, MAX_HDR - total_received);
            if (r <= 0) {
                close(client_fd);
                return; // 연결 종료 or 오류
            }
            total_received += r;
            buffer[total_received] = '\0';
            if (strstr(buffer, "\r\n\r\n")) break; // 헤더 끝 감지
        }

        if (total_received >= MAX_HDR) {
            write(client_fd, errMessage400, strlen(errMessage400));
            break;
        }

        char method[8], url[MAX_URL], version[16];
        if (sscanf(buffer, "%7s %1023s %15s", method, url, version) != 3 || strncmp(method, "GET", 3) != 0) {
            write(client_fd, errMessage400, strlen(errMessage400));
            break;
        }

        if (strstr(buffer, "Host:") == NULL) {
            write(client_fd, errMessage400, strlen(errMessage400));
            break;
        }

        int keep_alive = 0;
        if (is_connection_keep_alive(buffer)) {
            keep_alive = 1;
        } else if (is_connection_close(buffer)) {
            keep_alive = 0;
        } else {
           
            if (strstr(version, "HTTP/1.1")) keep_alive = 1;
        }

        char filepath[2048];
        snprintf(filepath, sizeof(filepath), "%s%s", g_rootDir, url);
        char *q = strchr(filepath, '?');
        if (q) *q = '\0';

        struct stat path_stat;
        if (stat(filepath, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
            strncat(filepath, "/index.html", sizeof(filepath) - strlen(filepath) - 1);
        }

        int file_fd = open(filepath, O_RDONLY);
        if (file_fd < 0) {
            write(client_fd, errMessage404, strlen(errMessage404));
            if (!keep_alive) break;
            else continue;
        }

        struct stat st;
        if (fstat(file_fd, &st) < 0) {
            write(client_fd, errMessage500, strlen(errMessage500));
            close(file_fd);
            if (!keep_alive) break;
            else continue;
        }

        dprintf(client_fd,
            "HTTP/1.0 200 OK\r\n"
            "Content-length: %ld\r\n"
            "Connection: %s\r\n"
            "\r\n",
            st.st_size, keep_alive ? "Keep-Alive" : "close");

        off_t offset = 0;
        ssize_t sent_total = 0;
        while (offset < st.st_size) {
            ssize_t sent = sendfile(client_fd, file_fd, &offset, st.st_size - offset);
            if (sent <= 0) break;
            sent_total += sent;
        }

        // fprintf(stderr, "[RESPONSE] sent: %ld (expected: %ld)\n", (long)sent_total, (long)st.st_size);

            

        close(file_fd);
        if (!keep_alive) break;
    }
    close(client_fd);
}

static void PrintUsage(const char* prog) {
    printf("usage: %s -p port -d rootDirectory(optional) \n", prog);
}

int main(const int argc, const char** argv) {
    int i;
    int port = -1;

    // Argument parsing
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && (i+1) < argc) {
            port = atoi(argv[i+1]);
            i++;
        } else if (strcmp(argv[i], "-d") == 0 && (i+1) < argc) {
            g_rootDir = argv[i+1];
            i++;
        }
    }
    if (port <= 0 || port > 65535) {
        PrintUsage(argv[0]);
        exit(-1);
    }
    if (access(g_rootDir, R_OK | X_OK) < 0) {
        fprintf(stderr, "root dir %s inaccessible, errno=%d\n", g_rootDir, errno);
        PrintUsage(argv[0]);
        exit(-1);
    }

    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);

    // Socket setup
    int listen_fd;
    struct sockaddr_in servaddr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        exit(1);
    }

    // Accept loop
    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t clilen = sizeof(cliaddr);
        int conn_fd = accept(listen_fd, (struct sockaddr*)&cliaddr, &clilen);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }

        if (fork() == 0) {
            close(listen_fd);
            handle_request(conn_fd);
            close(conn_fd);
            exit(0);
        }
        close(conn_fd);
        waitpid(-1, NULL, WNOHANG);
    }
}
