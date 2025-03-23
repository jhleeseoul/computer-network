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
}
