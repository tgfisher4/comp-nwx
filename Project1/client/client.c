/*
** client.c -- a stream socket client demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#define MAXDATASIZE 100 // max number of bytes we can get at once 

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[])
{
    int sockfd, numbytes;  
    char buf[MAXDATASIZE];
    struct addrinfo hints, *servinfo, *p;
    int rv;
    char s[INET6_ADDRSTRLEN];

    if (argc != 4) {
        fprintf(stderr,"[Error] Usage: %s host port filename\n", argv[0]);
        exit(1);
    }
    const char* hostname = argv[1];
    const char* port = argv[2];
    const char* filename = argv[3];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(hostname, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "[Error] Failed to getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and connect to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                             p->ai_protocol)) == -1) {
            perror("[Error] Failed to open socket");
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("[Error] Failed to connect to returned host");
            close(sockfd);
            continue;
        }

        break;
    }

    if (p == NULL) {
        fprintf(stderr, "[Error] Failed to connect to any host.");
        return 2;
    }

    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
            s, sizeof s);
    printf("[Info] Connecting to %s\n", s);

    freeaddrinfo(servinfo); // all done with this structure

    // send length of filename
    size_t filename_len = strlen(filename);
    if( filename_length >= (1 << 16) ){
        fprintf(stderr, "[Error] Filename is too long: must be shorter than 2^16 characters.");
        return -1;
    }
    uint16_t filename_len_16b = htons((uint16_t)filename_len);
    for( int sent = 0; sent < 2; ){
        int put = send(sockfd, &filename_len_16b + sent, 2 - sent, 0);
        if( put < 0 ) {
            perror("[Error] Failed to send filename length")
            return -1;
        }
        sent += put;
    }

    // send filename
    for( int sent = 0; sent < filename_sz; ){
        int put = send(sockfd, filename + sent, filename_sz - sent, 0);
        if( put < 0 ){
            perror("[Error] Failed to send filename size");
            return -1;
        }
        sent += put;
    }

    // receive file length
    # define file_len_len 4
    char file_len[file_len_len];
    for( int recvd = 0; recvd < file_len_len; ){
        int got = recv(sockfd, file_len + recvd, file_len_len - recvd, 0);
        if( got < 0 ){
            perror("[Error] Failed to receive file length");
            return -1
        }
        revd += got;
    }
    uint32_t file_len = ntohl(*(uint32_t *)buf);

    FILE *save_file = open(filename, "w");
    
    // start timer
    struct timeval start;
    if( gettimeofday(&start, NULL) < 0 ){
        perror("[Error] Failed to fetch start time");
        return -1;
    }

    // receive file
    int recvd = 0; // declare outside loop to have access later
    for( ; recvd < filesize; ){
        int got = recv(sockfd, buf, min(BUFSIZ, filesize - recvd), 0);
        if( got < 0 ){
            perror("[Error] Failed to receive file contents");
            return -1;
        }
        if( got == 0 ){
            fprintf(stderr, "[Error] File stream ended unexpectedly\n");
            return -1;
        }
        revd += got;
        // save file locally
        if( fwrite(buf, sizeof(char), got, save_file) < got ){
            perror"[Error] Fatal fwrite error");
        }
    }

    // stop timer
    struct timeval end;
    if( gettimeofday(&end, NULL) < 0 ){
        perror("[Error] Failed to fetch end time");
        return -1;
    }

    // output throughput statistics
    double transfer_time = (end.tv_usec - start.tv_usec)/1000000.0;
    if( tranfer_time < 0 ){
    end.tv_sec -= 1;
    transfer_time = 1 - transfer_time;
    }
    transfer_time += end.tv_sec - start.tv_sec;
    double throughput = recvd / transfer_time; // B/sec
    printf("[Success] %d bytes received in %fs (%f B/s)\n", recvd, transfer_time, throughput);

    close(sockfd);

    return 0;
}
