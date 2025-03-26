#include <stdio.h>
#include <errno.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/wait.h>

#include "macro.h"

static void handle_connection(int connfd);
static int  parse_request_header(
    char *headerBuf,
    size_t headerLen,
    size_t *contentLenOut
);
static void send_400_response(int connfd);

/*--------------------------------------------------------------------------------*/
int 
main(const int argc, const char** argv)
{
    int i;
    int port = -1;

    /* argument parsing */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && (i+1) < argc) {
            port = atoi(argv[i+1]);
            i++;
        }
    }
    if (port <= 0 || port > 65535) {
        printf("usage: %s -p port\n", argv[0]);
        exit(-1);
    }

    
    {

        signal(SIGPIPE, SIG_IGN);

        int s;
        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));

        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            perror("socket");
            exit(-1);
        }

        int optval = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        saddr.sin_family      = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        saddr.sin_port        = htons(port);

        if (bind(s, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
            perror("bind");
            close(s);
            exit(-1);
        }

        if (listen(s, 128) < 0) {
            perror("listen");
            close(s);
            exit(-1);
        }

        int num_children = 5;
        for (int c = 0; c < num_children; c++) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                close(s);
                exit(-1);
            } 
            else if (pid == 0) {
                while (1) {
                    struct sockaddr_in cliaddr;
                    socklen_t clilen = sizeof(cliaddr);
                    int connfd = accept(s, (struct sockaddr *)&cliaddr, &clilen);
                    if (connfd < 0) {
                        perror("accept");
                        continue;
                    }

                    handle_connection(connfd);

                    close(connfd);
                }
                exit(0);
            }
        }

        while (1) {
            int status = 0;
            pid_t w = waitpid(-1, &status, 0);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("waitpid");
                break;
            }
        }

        close(s);
        return 0;
    }
}

static void handle_connection(int connfd)
{

    char headerBuf[MAX_HDR + 1];
    memset(headerBuf, 0, sizeof(headerBuf));
    size_t used = 0;
    int headerComplete = 0;

    while (!headerComplete) {
        if (used >= MAX_HDR) {
            send_400_response(connfd);
            return;
        }
        ssize_t rn = read(connfd, headerBuf + used, 1);
        if (rn < 0) {
            perror("read");
            send_400_response(connfd);
            return;
        } 
        else if (rn == 0) {
            send_400_response(connfd);
            return;
        }
        used += rn;

        if (used >= 4 && strstr(headerBuf, "\r\n\r\n") != NULL) {
            headerComplete = 1;
        }
    }
    headerBuf[used] = '\0';

    char *pos = strstr(headerBuf, "\r\n\r\n");
    if (!pos) {
        send_400_response(connfd);
        return;
    }
    pos += 4;  
    size_t headerConsumed = (size_t)(pos - headerBuf); 
    size_t leftover = used - headerConsumed;

    size_t headerLen = headerConsumed - 4; 
    if (headerLen > MAX_HDR) {
        send_400_response(connfd);
        return;
    }

    char headerCopy[MAX_HDR + 1];
    memcpy(headerCopy, headerBuf, headerLen);
    headerCopy[headerLen] = '\0';

    size_t contentLen = 0;
    int ret = parse_request_header(headerCopy, headerLen, &contentLen);
    if (ret != 0) {
        send_400_response(connfd);
        return;
    }

    if (contentLen > MAX_CONT) {
        send_400_response(connfd);
        return;
    }

    unsigned char *bodyBuf = (unsigned char*)malloc(contentLen);
    if (!bodyBuf) {
        perror("malloc bodyBuf");
        send_400_response(connfd);
        return;
    }

    size_t received = 0;
    size_t copyLen = (leftover <= contentLen) ? leftover : contentLen;
    if (copyLen > 0) {
        memcpy(bodyBuf, headerBuf + headerConsumed, copyLen);
        received += copyLen;
    }

    
    while (received < contentLen) {
        ssize_t rn = read(connfd, bodyBuf + received, contentLen - received);
        if (rn < 0) {
            perror("read body");
            free(bodyBuf);
            send_400_response(connfd);
            return;
        }
        else if (rn == 0) {
            
            free(bodyBuf);
            send_400_response(connfd);
            return;
        }
        received += rn;
    }

    
    {
        char respHeader[256];
        snprintf(respHeader, sizeof(respHeader),
                 "SIMPLE/1.0 200 OK\r\n"
                 "Content-length: %zu\r\n"
                 "\r\n",
                 contentLen);

        
        size_t hdrLen = strlen(respHeader);
        size_t sent = 0;
        while (sent < hdrLen) {
            ssize_t wn = write(connfd, respHeader + sent, hdrLen - sent);
            if (wn < 0) {
                perror("write resp header");
                free(bodyBuf);
                return;
            }
            sent += wn;
        }

        
        sent = 0;
        while (sent < contentLen) {
            ssize_t wn = write(connfd, bodyBuf + sent, contentLen - sent);
            if (wn < 0) {
                perror("write resp body");
                free(bodyBuf);
                return;
            }
            sent += wn;
        }
    }

    free(bodyBuf);
}


static int parse_request_header(char *headerBuf, size_t headerLen, size_t *contentLenOut)
{
    
    headerBuf[headerLen] = '\0';

    char *saveptr = NULL;
    char *line = strtok_r(headerBuf, "\r\n", &saveptr);

    
    if (!line) {
        return -1;
    }

    {
        
        
        char *saveptr2 = NULL;
        char *token1 = strtok_r(line, " \t", &saveptr2);
        char *token2 = strtok_r(NULL, " \t", &saveptr2);
        char *token3 = strtok_r(NULL, " \t", &saveptr2);

        if (!token1 || !token2 || !token3) {
            return -1;
        }

        if (strcmp(token1, "POST") != 0) {
            return -1;
        }
        if (strcasecmp(token2, "message") != 0) {
            return -1;
        }
        if (strcasecmp(token3, "SIMPLE/1.0") != 0) {
            return -1;
        }
    }

    
    int foundHost = 0;
    int foundCL   = 0;
    *contentLenOut = 0;

    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {

        char *lower = strdup(line);
        if (!lower) continue;
        for (char *pp = lower; *pp; pp++)
            *pp = tolower((unsigned char)*pp);

        if (strstr(lower, "host:") == lower) {
            foundHost = 1;
        }
        else if (strstr(lower, "content-length:") == lower) {
            foundCL = 1;
            
            char *numPtr = strchr(line, ':');
            if (numPtr) {
                numPtr++; 
                while (*numPtr && isspace((unsigned char)*numPtr)) numPtr++;
                unsigned long long val = strtoull(numPtr, NULL, 10);
                *contentLenOut = (size_t)val;
            }
        }
        free(lower);
    }

    if (!foundHost || !foundCL) {
        return -1;
    }
    return 0; 
}

static void send_400_response(int connfd)
{
    const char *resp = 
        "SIMPLE/1.0 400 Bad Request\r\n"
        "\r\n";


    size_t len = strlen(resp);
    size_t sent = 0;
    while (sent < len) {
        ssize_t wn = write(connfd, resp + sent, len - sent);
        if (wn < 0) {
            perror("write 400 response");
            return;
        }
        sent += wn;
    }
}
