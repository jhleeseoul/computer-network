#include <stdio.h>
#include <sys/types.h>          /* See NOTES */
#include <netdb.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>

#include "macro.h"

/*--------------------------------------------------------------------------------*/
int 
main(const int argc, const char** argv) 
{
    const char *pserver = NULL;
    int port = -1;
    int i;
      
    /* argument processing */
    for (i = 1; i < argc; i++)  {
        if (strcmp(argv[i], "-p") == 0 && (i + 1) < argc) {
            port = atoi(argv[i+1]);
            i++;
        } else if (strcmp(argv[i], "-s") == 0 && (i + 1) < argc) {
            pserver = argv[i+1];
            i++;
        }
    }

    /* check arguments */
    if (port < 0 || pserver == NULL) {
        printf("usage: %s -p port -s server-ip\n", argv[0]);
        exit(-1);
    }
    if (port < 1024 || port > 65535) {
        printf("port number should be between 1024 ~ 65535.\n");
        exit(-1);
    }

    // implement your own code
    {
        /*----------------------------------------------------------------
         * 1. SIGPIPE 무시 (상대가 소켓을 닫아도 프로세스가 죽지 않게 함)
         *----------------------------------------------------------------*/
        signal(SIGPIPE, SIG_IGN);

        /*----------------------------------------------------------------
         * 2. stdin에서 최대 10MB까지 메시지 읽기
         *    - 10MB를 초과하면 나머지는 버림
         *    - 0바이트라면 에러 출력 후 종료
         *----------------------------------------------------------------*/
        unsigned char *sendBuf = (unsigned char *)malloc(MAX_CONT);
        if (!sendBuf) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }

        size_t totalRead = 0;
        while (1) {
            if (totalRead >= MAX_CONT) {
                /* 이미 10MB를 초과하면 나머지는 버리기 */
                char discardBuf[1024];
                if (!fread(discardBuf, 1, sizeof(discardBuf), stdin)) {
                    break;  // EOF or error
                }
                continue;
            }
            size_t canRead = MAX_CONT - totalRead;
            size_t n = fread(sendBuf + totalRead, 1, canRead, stdin);
            if (n == 0) {
                // EOF or error
                if (feof(stdin) || ferror(stdin)) {
                    break;
                }
            }
            totalRead += n;
        }

        if (totalRead == 0) {
            /* 입력이 전혀 없으면 에러 */
            fprintf(stderr, "Error: no input data (0 bytes)\n");
            free(sendBuf);
            exit(-1);
        }

        /*----------------------------------------------------------------
         * 3. 소켓 생성 및 서버 연결
         *----------------------------------------------------------------*/
        int sockfd;
        struct sockaddr_in servaddr;
        memset(&servaddr, 0, sizeof(servaddr));

        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("socket");
            free(sendBuf);
            exit(-1);
        }

        servaddr.sin_family = AF_INET;
        servaddr.sin_port   = htons(port);

        if (inet_pton(AF_INET, pserver, &servaddr.sin_addr) <= 0) {
            fprintf(stderr, "Invalid server IP address.\n");
            free(sendBuf);
            close(sockfd);
            exit(-1);
        }

        if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
            perror("connect");
            free(sendBuf);
            close(sockfd);
            exit(-1);
        }

        /*----------------------------------------------------------------
         * 4. 요청(request) 헤더를 작성하여 전송
         *    - 과제 사양에 맞추어 SIMPLE/1.0 프로토콜 사용
         *    - CRLF(\r\n) 처리 주의
         *----------------------------------------------------------------*/
        char header[2048];
        snprintf(header, sizeof(header),
                 "POST message SIMPLE/1.0\r\n"
                 "Host: %s\r\n"
                 "Content-length: %zu\r\n"
                 "\r\n",               /* 헤더와 본문 사이의 빈 줄 */
                 pserver, totalRead);

        /* 헤더 전송 (write loop 사용 권장) */
        {
            size_t sentBytes = 0;
            size_t headerLen = strlen(header);
            while (sentBytes < headerLen) {
                ssize_t wn = write(sockfd, header + sentBytes, headerLen - sentBytes);
                if (wn < 0) {
                    perror("write header");
                    free(sendBuf);
                    close(sockfd);
                    exit(-1);
                }
                sentBytes += wn;
            }
            fprintf(stderr, "DEBUG: sclient totalRead=%zu, final sentBytes=%zu\n", totalRead, sentBytes);
        }

        /* 본문 전송 (write loop) */
        {
            size_t sentBytes = 0;
            while (sentBytes < totalRead) {
                ssize_t wn = write(sockfd, sendBuf + sentBytes, totalRead - sentBytes);
                if (wn < 0) {
                    perror("write body");
                    free(sendBuf);
                    close(sockfd);
                    exit(-1);
                }
                sentBytes += wn;
            }
        }

        free(sendBuf);

        /*----------------------------------------------------------------
         * 5. 서버 응답(response) 수신 및 처리
         *    - 최대 헤더 1024바이트까지 읽어서 파싱
         *    - "SIMPLE/1.0 200 OK"라면, Content-length만큼 바디를 읽어
         *      stdout으로 출력
         *    - 그 외(400 Bad Request 등) 에러 응답이면 헤더만 출력
         *----------------------------------------------------------------*/
        char respHeader[MAX_HDR + 1];
        memset(respHeader, 0, sizeof(respHeader));
        size_t headerUsed = 0;
        int headerComplete = 0;

        /* (A) 헤더 부분 수신: "\r\n\r\n"까지 */
        while (!headerComplete) {
            if (headerUsed >= MAX_HDR) {
                // 헤더가 너무 큼 -> 에러 처리
                fprintf(stderr, "Error: response header exceeds %d bytes\n", MAX_HDR);
                close(sockfd);
                exit(-1);
            }
            ssize_t rn = read(sockfd, respHeader + headerUsed, 1);
            if (rn < 0) {
                perror("read");
                close(sockfd);
                exit(-1);
            } else if (rn == 0) {
                // 서버가 일찍 닫음 -> 전체 헤더가 오지 않았을 수 있음
                break;
            }
            headerUsed += rn;
            if (headerUsed >= 4 &&
                strstr(respHeader, "\r\n\r\n") != NULL) {
                /* "\r\n\r\n" 발견 -> 헤더 끝 */
                headerComplete = 1;
            }
        }
        respHeader[headerUsed] = '\0'; // 문자열 마무리

        /* (B) 첫 줄 파싱: "SIMPLE/1.0 200 OK" 인지 확인 */
        // 줄 단위로 파싱하기 위해 복사본 사용
        int is200 = 0;
        size_t contentLength = 0;
        {
            char *lines = strdup(respHeader);
            if (!lines) {
                fprintf(stderr, "strdup fail\n");
                close(sockfd);
                exit(-1);
            }
            char *saveptr = NULL;
            char *line = strtok_r(lines, "\r\n", &saveptr);
            // 첫 번째 줄 체크
            if (line) {
                // 대략적으로 SIMPLE/1.0 200 OK 인지 확인 (공백 허용)
                // 가장 간단히는 strstr() 등으로 체크 가능
                // 여기서는 토큰화 예시:
                char *p = line;
                // 공백을 전혀 안 넣었다고 가정하면,
                // "SIMPLE/1.0 200 OK" 형태 (앞에 공백 없다고 했음)
                if (strncmp(p, "SIMPLE/1.0", 10) == 0) {
                    // 뒤쪽 토큰에서 200이면 OK 처리
                    // 실제로는 더 정교한 공백 처리 필요
                    if (strstr(line, "200") != NULL && strstr(line, "OK") != NULL) {
                        is200 = 1;
                    }
                }
            }

            // 이어지는 헤더 라인들에서 Content-length 찾기
            while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
                // 소문자로 변환한 뒤에 "content-length:"가 있는지 확인
                char *lower = strdup(line);
                if (!lower) continue;
                for (char *pp = lower; *pp; pp++)
                    *pp = tolower((unsigned char)*pp);

                // "content-length:" 포함?
                if (strstr(lower, "content-length:") != NULL) {
                    // 실제 line에서 숫자 파싱
                    char *numPtr = strchr(line, ':');
                    if (numPtr) {
                        numPtr++; // ':' 뒤
                        // 앞뒤 공백 제거
                        while (*numPtr && isspace((unsigned char)*numPtr)) numPtr++;
                        contentLength = strtoull(numPtr, NULL, 10);
                    }
                }
                free(lower);
            }
            free(lines);
        }

        /* (C) 헤더가 "200 OK" 라면, body만큼 읽어서 stdout에 출력 */
        if (is200) {
            // 현재까지 read()로 헤더 일부만 읽었고, 그 사이에 body 데이터가
            // 이미 도착했을 수 있으므로, respHeader + headerUsed ~ buffer 끝
            // 을 확인하여 body가 섞여 있으면 처리해야 함.
            // 여기서는 간단히 "\r\n\r\n" 뒤를 찾아 남은 바이트만큼을 먼저 처리.
            char *bodyPos = strstr(respHeader, "\r\n\r\n");
            size_t leftover = 0;
            unsigned char *bodyStart = NULL;
            if (bodyPos) {
                bodyPos += 4; // "\r\n\r\n" 건너뛰기
                leftover = headerUsed - (bodyPos - respHeader);
                bodyStart = (unsigned char*)bodyPos;
            }

            // leftover 만큼 먼저 stdout에 출력
            size_t writtenLeft = 0;
            while (writtenLeft < leftover) {
                ssize_t wn = write(STDOUT_FILENO, bodyStart + writtenLeft, leftover - writtenLeft);
                if (wn < 0) {
                    perror("write to stdout");
                    close(sockfd);
                    exit(-1);
                }
                writtenLeft += wn;
            }

            // leftover를 제외하고 더 읽어야 할 바이트
            size_t remain = (contentLength > leftover) ? (contentLength - leftover) : 0;

            // 나머지 remain 바이트를 소켓에서 읽어서 stdout에 출력
            size_t received = 0;
            unsigned char buf[4096];
            while (received < remain) {
                size_t toRead = (remain - received) > sizeof(buf) ? sizeof(buf) : (remain - received);
                ssize_t rn = read(sockfd, buf, toRead);
                if (rn < 0) {
                    perror("read body");
                    close(sockfd);
                    exit(-1);
                } else if (rn == 0) {
                    // body를 다 못받고 서버 종료
                    break;
                }
                // stdout에 쓰기
                size_t written = 0;
                while (written < (size_t)rn) {
                    ssize_t wn = write(STDOUT_FILENO, buf + written, rn - written);
                    if (wn < 0) {
                        perror("write to stdout");
                        close(sockfd);
                        exit(-1);
                    }
                    written += wn;
                }
                received += rn;
            }
        }
        else {
            /* (D) 에러 응답(400 등)이면 헤더 전체를 그대로 stdout에 출력 */
            // 이미 respHeader에 다 있으므로 그대로 출력
            // (헤더 끝 이후의 바디는 사양상 없음)
            write(STDOUT_FILENO, respHeader, headerUsed);
        }

        close(sockfd);
        return 0;
    }
}
