FILES=src/blob.c src/worker.c src/util.c src/throttle.c
RELAY=src/relay.c $(FILES)
CLIENT=src/client.c $(FILES)
CC=gcc -O3 -Wall -pthread -g -DMAX_WORKERS=2
all:
	mkdir -p bin
	$(CC) -o bin/client $(CLIENT)
	$(CC) -DUSE_SERVER -o bin/server $(CLIENT)
	$(CC) -o bin/relay $(RELAY)

clean:
	rm -f bin/relay* test/sock/*
	
run:
	cd test && ./setup.sh