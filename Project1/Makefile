all: client server

clean:
	rm -f client/proj1client server/proj2server

.PHONY: client server

client: client/client.c
	gcc client/client.c -std=gnu99 -Wall -o client/proj1client

server: server/server.c
	gcc server/server.c -std=gnu99 -Wall -o server/proj1server
