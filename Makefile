CC = gcc
CFLAGS =  -O3 -march=native -std=gnu11 -I. -lpulse -lsndfile
DEPS = 

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)


%: %.c 
	$(CC) -o $@ $^ $(CFLAGS)

all: saplay
