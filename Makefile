CC      = gcc
CFLAGS  = -Wall -Wextra -g

SERVER  = hangman_server
CLIENT  = hangman_client

SRV_SRC = hangman_server.c
CLI_SRC = hangman_client.c

all: $(SERVER) $(CLIENT)

$(SERVER): $(SRV_SRC)
	$(CC) $(CFLAGS) -pthread -o $(SERVER) $(SRV_SRC)

$(CLIENT): $(CLI_SRC)
	$(CC) $(CFLAGS) -o $(CLIENT) $(CLI_SRC)

clean:
	rm -f $(SERVER) $(CLIENT) *.o
