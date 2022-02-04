all: client server

.PHONY: client server

client: client/client.c
	gcc client/client.c -std=gnu99 -Wall -o client/client

server: server/server.c
	gcc server/server.c -std=gnu99 -Wall -o server/server
