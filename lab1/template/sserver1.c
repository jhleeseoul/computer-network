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

/*-----------------------------------------------------------------------------
 * 함수 선언 (static)
 *---------------------------------------------------------------------------*/
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

    // implement your own code
    {
        /*-------------------------------------------------------------
         * 1. SIGPIPE 무시
         *-------------------------------------------------------------*/
        signal(SIGPIPE, SIG_IGN);
        fprintf(stderr, "[DEBUG] sserver: Starting server on port %d\n", port);

        /*-------------------------------------------------------------
         * 2. 소켓 생성 및 바인드/리스닝
         *-------------------------------------------------------------*/
        int sockfd;
        struct sockaddr_in servaddr;
        memset(&servaddr, 0, sizeof(servaddr));

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("socket");
            exit(-1);
        }
        fprintf(stderr, "[DEBUG] sserver: socket created (fd=%d)\n", sockfd);

        int optval = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        servaddr.sin_family      = AF_INET;
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.sin_port        = htons(port);

        if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
            perror("bind");
            close(sockfd);
            exit(-1);
        }
        fprintf(stderr, "[DEBUG] sserver: bind successful\n");

        if (listen(sockfd, 128) < 0) {
            perror("listen");
            close(sockfd);
            exit(-1);
        }
        fprintf(stderr, "[DEBUG] sserver: listen on port %d\n", port);

        /*-------------------------------------------------------------
         * 3. prefork 방식으로 동시 5개의 프로세스가 accept() 대기
         *-------------------------------------------------------------*/
        int num_children = 5;
        for (int c = 0; c < num_children; c++) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                close(sockfd);
                exit(-1);
            } 
            else if (pid == 0) {
                /* child process */
                fprintf(stderr, "[DEBUG] sserver: child %d started (PID=%d)\n", c, (int)getpid());
                while (1) {
                    struct sockaddr_in cliaddr;
                    socklen_t clilen = sizeof(cliaddr);
                    int connfd = accept(sockfd, (struct sockaddr *)&cliaddr, &clilen);
                    if (connfd < 0) {
                        perror("accept");
                        continue;
                    }
                    fprintf(stderr, "[DEBUG] child PID=%d: accepted connection (connfd=%d)\n", (int)getpid(), connfd);

                    handle_connection(connfd);

                    close(connfd);
                    fprintf(stderr, "[DEBUG] child PID=%d: connection closed (connfd=%d)\n", (int)getpid(), connfd);
                }
                exit(0);
            }
            // 부모는 계속 for 루프 진행
        }

        /*-------------------------------------------------------------
         * 4. parent process: 자식들이 돌고 있는 상태에서 대기/관리
         *-------------------------------------------------------------*/
        fprintf(stderr, "[DEBUG] sserver: parent (PID=%d) waiting children\n", (int)getpid());
        while (1) {
            int status = 0;
            pid_t w = waitpid(-1, &status, 0);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("waitpid");
                break;
            }
            fprintf(stderr, "[DEBUG] sserver: child (PID=%d) terminated\n", (int)w);
        }

        close(sockfd);
        return 0;
    }
}

/*-----------------------------------------------------------------------------
 * handle_connection: 클라이언트 연결 하나를 처리
 *   1) 최대 1024바이트 헤더 읽기 & 파싱
 *   2) Content-length만큼 본문 수신
 *   3) 정상 형식이면 200 응답, 아니면 400 응답
 *---------------------------------------------------------------------------*/
static void handle_connection(int connfd)
{
    fprintf(stderr, "[DEBUG] handle_connection(connfd=%d) start\n", connfd);

    char headerBuf[MAX_HDR + 1];
    memset(headerBuf, 0, sizeof(headerBuf));
    size_t used = 0;
    int headerComplete = 0;

    // (1) 헤더 읽기
    while (!headerComplete) {
        if (used >= MAX_HDR) {
            fprintf(stderr, "[DEBUG] handle_connection: header too large\n");
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
            // 클라이언트가 헤더도 다 안 보내고 종료
            fprintf(stderr, "[DEBUG] handle_connection: client closed early\n");
            send_400_response(connfd);
            return;
        }
        used += rn;

        // "\r\n\r\n" 검출
        if (used >= 4 && strstr(headerBuf, "\r\n\r\n") != NULL) {
            headerComplete = 1;
        }
    }
    headerBuf[used] = '\0';
    fprintf(stderr, "[DEBUG] handle_connection: read header complete (used=%zu)\n", used);

    // (2) leftover 계산(파싱 전에)
    char *pos = strstr(headerBuf, "\r\n\r\n");
    if (!pos) {
        // 이론상 위에서 headerComplete=1이 되려면 pos가 있어야 하는데,
        // 만약 NULL이면 뭔가 이상하니 400 처리
        fprintf(stderr, "[DEBUG] handle_connection: cannot find CRLFCRLF even though headerComplete=1?\n");
        send_400_response(connfd);
        return;
    }
    // body 시작 위치
    pos += 4;  // "\r\n\r\n" 길이만큼 건너뛰기
    size_t headerConsumed = (size_t)(pos - headerBuf);  // 헤더(포함 CRLFCRLF) 길이
    size_t leftover = used - headerConsumed;
    fprintf(stderr, "[DEBUG] leftover=%zu (used=%zu, headerConsumed=%zu)\n", leftover, used, headerConsumed);

    // (3) 헤더 파싱은 복사본 사용
    // parse_request_header에서 strtok_r로 '\0' 치환해도 원본 buffer의 CRLFCRLF 자리가 안 망가지도록
    // 헤더 부분만 복사
    size_t headerLen = headerConsumed - 4; // 실제 헤더(마지막 "\r\n\r\n" 직전까지)
    if (headerLen > MAX_HDR) {
        // 이론상 없겠지만 안정성 차원
        fprintf(stderr, "[DEBUG] handle_connection: headerLen too large\n");
        send_400_response(connfd);
        return;
    }

    char headerCopy[MAX_HDR + 1];
    memcpy(headerCopy, headerBuf, headerLen);
    headerCopy[headerLen] = '\0';

    size_t contentLen = 0;
    int ret = parse_request_header(headerCopy, headerLen, &contentLen);
    if (ret != 0) {
        fprintf(stderr, "[DEBUG] handle_connection: parse_request_header -> error\n");
        send_400_response(connfd);
        return;
    }
    fprintf(stderr, "[DEBUG] handle_connection: parsed contentLen=%zu\n", contentLen);

    // 10MB 초과 검사
    if (contentLen > MAX_CONT) {
        fprintf(stderr, "[DEBUG] handle_connection: contentLen > MAX_CONT => 400\n");
        send_400_response(connfd);
        return;
    }

    // (4) leftover 부분을 먼저 bodyBuf에 복사
    unsigned char *bodyBuf = (unsigned char*)malloc(contentLen);
    if (!bodyBuf) {
        perror("malloc bodyBuf");
        send_400_response(connfd);
        return;
    }

    size_t received = 0;
    // leftover가 본문보다 많으면 그만큼만 복사
    size_t copyLen = (leftover <= contentLen) ? leftover : contentLen;
    if (copyLen > 0) {
        memcpy(bodyBuf, headerBuf + headerConsumed, copyLen);
        received += copyLen;
        fprintf(stderr, "[DEBUG] handle_connection: leftover copyLen=%zu => received=%zu\n", copyLen, received);
    }

    // (5) 나머지 body 수신
    while (received < contentLen) {
        ssize_t rn = read(connfd, bodyBuf + received, contentLen - received);
        if (rn < 0) {
            perror("read body");
            free(bodyBuf);
            send_400_response(connfd);
            return;
        }
        else if (rn == 0) {
            // 중간에 끊김
            fprintf(stderr, "[DEBUG] handle_connection: client closed mid-body => 400\n");
            free(bodyBuf);
            send_400_response(connfd);
            return;
        }
        received += rn;
        fprintf(stderr, "[DEBUG] handle_connection: body read rn=%zd => received=%zu\n", rn, received);
    }

    // (6) 응답 전송(200 OK + body)
    fprintf(stderr, "[DEBUG] handle_connection: body fully received => sending response\n");
    {
        char respHeader[256];
        snprintf(respHeader, sizeof(respHeader),
                 "SIMPLE/1.0 200 OK\r\n"
                 "Content-length: %zu\r\n"
                 "\r\n",
                 contentLen);

        // 헤더 전송
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

        // 본문 전송
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
    fprintf(stderr, "[DEBUG] handle_connection: done (connfd=%d)\n", connfd);
}


/*-----------------------------------------------------------------------------
 * parse_request_header:
 *   1) 첫 줄이 "POST message SIMPLE/1.0" 형식인지 확인
 *   2) Host: 필드 존재 여부 확인
 *   3) Content-length: 필드 찾아서 숫자 파싱
 *   4) 기타 포맷 이상 시 에러 -> 리턴 -1
 *   정상 시 0
 *---------------------------------------------------------------------------*/
static int parse_request_header(char *headerBuf, size_t headerLen, size_t *contentLenOut)
{
    // 안전을 위해 널 종결 (상위에서 이미 널종결 했음)
    headerBuf[headerLen] = '\0';
    fprintf(stderr, "[DEBUG] parse_request_header: headerLen=%zu\n", headerLen);

    char *saveptr = NULL;
    char *line = strtok_r(headerBuf, "\r\n", &saveptr);

    // 첫 줄 확인
    if (!line) {
        fprintf(stderr, "[DEBUG] parse_request_header: no first line -> error\n");
        return -1;
    }
    fprintf(stderr, "[DEBUG] parse_request_header: first line=\"%s\"\n", line);

    {
        // 공백 단위 토큰화
        // "POST message SIMPLE/1.0" 형태
        char *saveptr2 = NULL;
        char *token1 = strtok_r(line, " \t", &saveptr2);
        char *token2 = strtok_r(NULL, " \t", &saveptr2);
        char *token3 = strtok_r(NULL, " \t", &saveptr2);

        if (!token1 || !token2 || !token3) {
            fprintf(stderr, "[DEBUG] parse_request_header: missing tokens -> error\n");
            return -1;
        }

        if (strcmp(token1, "POST") != 0) {
            fprintf(stderr, "[DEBUG] parse_request_header: first token not POST -> error\n");
            return -1;
        }
        if (strcasecmp(token2, "message") != 0) {
            fprintf(stderr, "[DEBUG] parse_request_header: second token not 'message' -> error\n");
            return -1;
        }
        if (strcasecmp(token3, "SIMPLE/1.0") != 0) {
            fprintf(stderr, "[DEBUG] parse_request_header: third token not 'SIMPLE/1.0' -> error\n");
            return -1;
        }
    }

    // 나머지 라인들에서 Host:와 Content-length: 찾기
    int foundHost = 0;
    int foundCL   = 0;
    *contentLenOut = 0;

    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
        fprintf(stderr, "[DEBUG] parse_request_header: next line=\"%s\"\n", line);

        char *lower = strdup(line);
        if (!lower) continue;
        for (char *pp = lower; *pp; pp++)
            *pp = tolower((unsigned char)*pp);

        if (strstr(lower, "host:") == lower) {
            foundHost = 1;
            fprintf(stderr, "[DEBUG] parse_request_header: found Host\n");
        }
        else if (strstr(lower, "content-length:") == lower) {
            fprintf(stderr, "[DEBUG] parse_request_header: found Content-length\n");
            foundCL = 1;
            // 실제 line에서 ':' 뒤 찾아 숫자 파싱
            char *numPtr = strchr(line, ':');
            if (numPtr) {
                numPtr++; // ':' 뒤
                while (*numPtr && isspace((unsigned char)*numPtr)) numPtr++;
                unsigned long long val = strtoull(numPtr, NULL, 10);
                *contentLenOut = (size_t)val;
                fprintf(stderr, "[DEBUG] parse_request_header: parsed contentLen=%zu\n", *contentLenOut);
            }
        }
        free(lower);
    }

    if (!foundHost || !foundCL) {
        fprintf(stderr, "[DEBUG] parse_request_header: missing Host: or Content-length:\n");
        return -1;
    }
    return 0; // 정상
}

/*-----------------------------------------------------------------------------
 * send_400_response: 잘못된 요청에 대한 400 응답 전송
 *---------------------------------------------------------------------------*/
static void send_400_response(int connfd)
{
    const char *resp = 
        "SIMPLE/1.0 400 Bad Request\r\n"
        "\r\n";

    fprintf(stderr, "[DEBUG] send_400_response(connfd=%d)\n", connfd);

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
