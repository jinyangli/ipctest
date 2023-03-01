CC=g++
all: main shmem
MAIN_OBJS = mqueue_vs_pipe.o 
SHMEM_OBJS = shmem.o

main: $(MAIN_OBJS)
	$(CC) -o $@ $^ -lrt

shmem: $(SHMEM_OBJS)
	$(CC) -o $@ $^ -lrt -lpthread

%.o : %.cc %.h
	$(CC) $(CFLAGS) -c ${<}

clean:
	rm -f *~ *.o main mqueue_vs_pipe.o shmem.o

