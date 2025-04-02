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

    
    {

        signal(SIGPIPE, SIG_IGN);

        unsigned char *sendBuf = (unsigned char *)malloc(MAX_CONT);
        if (!sendBuf) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }

        size_t totalRead = 0;
        while (1) {
            if (totalRead >= MAX_CONT) {
                char discardBuf[1024];
                if (!fread(discardBuf, 1, sizeof(discardBuf), stdin)) {
                    break;  
                }
                continue;
            }
            size_t canRead = MAX_CONT - totalRead;
            size_t n = fread(sendBuf + totalRead, 1, canRead, stdin);
            if (n == 0) {
                
                if (feof(stdin) || ferror(stdin)) {
                    break;
                }
            }
            totalRead += n;
        }

        if (totalRead == 0) {
            fprintf(stderr, "Error: no input data (0 bytes)\n");
            free(sendBuf);
            exit(-1);
        }

        int s;
        struct sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));

        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            perror("socket");
            free(sendBuf);
            exit(-1);
        }

        saddr.sin_family = AF_INET;
        saddr.sin_port   = htons(port);

        if (inet_pton(AF_INET, pserver, &saddr.sin_addr) <= 0) {
            fprintf(stderr, "Invalid server IP address.\n");
            free(sendBuf);
            close(s);
            exit(-1);
        }

        if (connect(s, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
            perror("connect");
            free(sendBuf);
            close(s);
            exit(-1);
        }

        char header[2048];
        snprintf(header, sizeof(header),
                 "POST message SIMPLE/1.0\r\n"
                 "Host: %s\r\n"
                 "Content-length: %zu\r\n"
                 "\r\n",              
                 pserver, totalRead);

        {
            size_t sentBytes = 0;
            size_t headerLen = strlen(header);
            while (sentBytes < headerLen) {
                ssize_t wn = write(s, header + sentBytes, headerLen - sentBytes);
                if (wn < 0) {
                    perror("write header");
                    free(sendBuf);
                    close(s);
                    exit(-1);
                }
                sentBytes += wn;
            }
        }

        {
            size_t sentBytes = 0;
            while (sentBytes < totalRead) {
                ssize_t wn = write(s, sendBuf + sentBytes, totalRead - sentBytes);
                if (wn < 0) {
                    perror("write body");
                    free(sendBuf);
                    close(s);
                    exit(-1);
                }
                sentBytes += wn;
            }
        }

        free(sendBuf);

        char respHeader[MAX_HDR + 1];
        memset(respHeader, 0, sizeof(respHeader));
        size_t headerUsed = 0;
        int headerComplete = 0;

        while (!headerComplete) {
            if (headerUsed >= MAX_HDR) {
                
                fprintf(stderr, "Error: response header exceeds %d bytes\n", MAX_HDR);
                close(s);
                exit(-1);
            }
            ssize_t rn = read(s, respHeader + headerUsed, 1);
            if (rn < 0) {
                perror("read");
                close(s);
                exit(-1);
            } else if (rn == 0) {
                
                break;
            }
            headerUsed += rn;
            if (headerUsed >= 4 &&
                strstr(respHeader, "\r\n\r\n") != NULL) {
                headerComplete = 1;
            }
        }
        respHeader[headerUsed] = '\0'; 

        
        int is200 = 0;
        size_t contentLength = 0;
        {
            char *lines = strdup(respHeader);
            if (!lines) {
                fprintf(stderr, "strdup fail\n");
                close(s);
                exit(-1);
            }
            char *saveptr = NULL;
            char *line = strtok_r(lines, "\r\n", &saveptr);
            
            if (line) {
                
                char *p = line;
                
                if (strncmp(p, "SIMPLE/1.0", 10) == 0) {
                    
                    
                    if (strstr(line, "200") != NULL && strstr(line, "OK") != NULL) {
                        is200 = 1;
                    }
                }
            }
            
            while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
                
                char *lower = strdup(line);
                if (!lower) continue;
                for (char *pp = lower; *pp; pp++)
                    *pp = tolower((unsigned char)*pp);

                
                if (strstr(lower, "content-length:") != NULL) {
                    
                    char *numPtr = strchr(line, ':');
                    if (numPtr) {
                        numPtr++; 
                        
                        while (*numPtr && isspace((unsigned char)*numPtr)) numPtr++;
                        contentLength = strtoull(numPtr, NULL, 10);
                    }
                }
                free(lower);
            }
            free(lines);
        }

        if (is200) {
            
            char *bodyPos = strstr(respHeader, "\r\n\r\n");
            size_t leftover = 0;
            unsigned char *bodyStart = NULL;
            if (bodyPos) {
                bodyPos += 4; 
                leftover = headerUsed - (bodyPos - respHeader);
                bodyStart = (unsigned char*)bodyPos;
            }

            
            size_t writtenLeft = 0;
            while (writtenLeft < leftover) {
                ssize_t wn = write(STDOUT_FILENO, bodyStart + writtenLeft, leftover - writtenLeft);
                if (wn < 0) {
                    perror("write to stdout");
                    close(s);
                    exit(-1);
                }
                writtenLeft += wn;
            }

            
            size_t remain = (contentLength > leftover) ? (contentLength - leftover) : 0;

            
            size_t received = 0;
            unsigned char buf[4096];
            while (received < remain) {
                size_t toRead = (remain - received) > sizeof(buf) ? sizeof(buf) : (remain - received);
                ssize_t rn = read(s, buf, toRead);
                if (rn < 0) {
                    perror("read body");
                    close(s);
                    exit(-1);
                } else if (rn == 0) {
                    
                    break;
                }
                
                size_t written = 0;
                while (written < (size_t)rn) {
                    ssize_t wn = write(STDOUT_FILENO, buf + written, rn - written);
                    if (wn < 0) {
                        perror("write to stdout");
                        close(s);
                        exit(-1);
                    }
                    written += wn;
                }
                received += rn;
            }
        }
        else {              
            write(STDOUT_FILENO, respHeader, headerUsed);
        }

        close(s);
        return 0;
    }
}
