/*
** server.c -- a stream socket server demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#define BACKLOG 10   // how many pending connections queue will hold
#define filename_len_len sizeof(uint16_t)
#define file_len_len sizeof(uint32_t)


void sigchld_handler(int s)
{
    (void)s; // quiet unused variable warning

    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int client_handler(int clientsockfd);

int main(int argc, char* argv[])
{
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;
    char s[INET6_ADDRSTRLEN];
    int rv;

    if( argc != 2 ){
        fprintf(stderr, "[Error] Usage: %s port", argv[0]);
    }
    char *port = argv[1];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "[Error] Failed to getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("[Error] Failed to create socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("[Error] Failed to setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("[Error] Failed to bind a socket");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo); // all done with this structure

    if (p == NULL)  {
        fprintf(stderr, "[Error] Failed to bind any socket");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1) {
        perror("[Error] Failed to listen");
        exit(1);
    }

    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("[Error] Failed to set sigaction");
        exit(1);
    }

    printf("[INFO] Waiting for connections...\n");

    while(1) {  // main accept() loop
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            perror("[Error] Failed to accept connection");
            continue;
        }

        inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s, sizeof s);
        printf("[INFO] Accepted connection from %s\n", s);

        if (!fork()) { // this is the child process
            close(sockfd); // child doesn't need the listener
            // receive filename len
            int rc = client_handler(new_fd);
            close(new_fd);
            exit(rc);
        }
        close(new_fd);  // parent doesn't need this
    }

    return 0;
}

int client_handler(int sockfd){
    // receive filename len
    uint16_t filename_len;
    for( size_t recvd = 0; recvd < filename_len_len; ){
        int got = recv(sockfd, (void *)&filename_len + recvd, filename_len_len - recvd, 0);
        if( got < 0 ){
            perror("[Error] Failed to receive file length");
            return -1;
        }
        recvd += got;
    }
    filename_len = ntohs(filename_len);

    // receive filename
    char *filename = malloc((size_t)filename_len);
    size_t recvd = 0;
    for( ; recvd < filename_len; ){
        int got = recv(sockfd, filename + recvd, filename_len - recvd, 0);
        if( got < 0 ){
            perror("[Error] Failed to receive file contents");
            return -1;
        }
        if( got == 0 ){
            fprintf(stderr, "[Error] Filename stream ended unexpectedly (got %zu, expected %d)\n", recvd, filename_len);
            return -1;
        }
        recvd += got; // adding signed to unsigned: should be fine as long as positive (negative is 2s complement: large)
    }

    // send file length
    FILE *file = fopen(filename, "r");
    if( !file ){
        perror("[Error] Could not open file to transfer");
        return -1;
    }

    fseek(file, 0, SEEK_END);
    size_t file_len = ftell(file);
    if( file_len >= (1UL << 32) ){
        fprintf(stderr, "[Error] File %s too large to be sent (greater than 2^32 B)\n", filename);
        return -1;
    }
    uint32_t file_len_32b = htonl((uint32_t)file_len);
    for( size_t sent = 0; sent < file_len_len; ){
        // cast uint32_t* to void* to have access to raw ptr arithmetic
        int put = send(sockfd, (void *)&file_len_32b + sent, file_len_len - sent, 0);
        if( put < 0 ) {
            perror("[Error] Failed to send filename size");
            return -1;
        }
        sent += put;
    }

    // send file
    fseek(file, 0, SEEK_SET);
    char buf[BUFSIZ];
    int got;
    while( (got = fread(buf, sizeof(char), BUFSIZ, file)) ){
        if( got < 0 ){
            perror("[Error] Failed to read file chunk");
            return -1;
        }

        for( size_t sent = 0; sent < got; ){
            int put = send(sockfd, buf + sent, BUFSIZ - sent, 0);
            if( put < 0 ) {
                perror("[Error] Failed to send file chunk");
                return -1;
            }
            sent += put;
        }
    }
    printf("[INFO] Successfully sent %s.\n", filename);
    return 0;
}
