#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

#include <pthread.h>
#include <semaphore.h>
#include <iostream>

#include <assert.h>
#include <string.h>

#include <sched.h>

struct Message {
    timespec ts;
};

struct BufferHeader {
    sem_t shm_semaphore_empty;
    sem_t shm_semaphore_nonempty;
    size_t begin;
    size_t end;
};

//buffer consists of header followed by a ring buffer of Messages
void *buffer_raw;
size_t buffer_cap = 2; //default ring buffer size is 2
BufferHeader *buffer_hdr;
Message *buffer_ring;
int parent_cpu = 0;
int child_cpu = 0;

void send_one_message() {
    Message msg;
    sem_wait(&(buffer_hdr->shm_semaphore_empty));
    assert(clock_gettime(CLOCK_REALTIME, &(msg.ts)) == 0);
    buffer_ring[buffer_hdr->end] = msg;
    buffer_hdr->end = (buffer_hdr->end +1) % buffer_cap;
    sem_post(&(buffer_hdr->shm_semaphore_nonempty));
}


#define BATCH 2000
#define NREPEAT 1000
void sender() {
    for (int i = 0; i < NREPEAT*BATCH; i++) {
        send_one_message();
    }
}

inline int64_t time_elapsed(const timespec& start, const timespec& end) {
    return (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
}

int64_t recv_one_message() {
    int64_t lat;
    timespec ts;
    sem_wait(&(buffer_hdr->shm_semaphore_nonempty));
    assert(clock_gettime(CLOCK_REALTIME, &ts) ==0);
    lat = time_elapsed(buffer_ring[buffer_hdr->begin].ts, ts);
    buffer_hdr->begin = (buffer_hdr->begin + 1) % buffer_cap;
    sem_post(&(buffer_hdr->shm_semaphore_empty));
    return lat;
}

void receiver() {
    for (int i = 0; i < NREPEAT; i++) {
        int64_t sum_lat = 0;
        int64_t max_lat = 0;
        for (int j = 0; j < BATCH; j++) {
            int lat = recv_one_message();
            sum_lat += lat;
            if (lat > max_lat)
                max_lat = lat;
        }
        std::cout << "Avg Lat " << sum_lat/BATCH << " max lat " << max_lat << std::endl;
    }
}

void init_buffer(bool shared_memory) {
    if (shared_memory) {
        buffer_raw = mmap(NULL, sizeof(BufferHeader) + sizeof(Message)*buffer_cap,\
            PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    } else {
        buffer_raw = malloc(sizeof(BufferHeader) + sizeof(Message)*buffer_cap);
    }
    buffer_hdr = (BufferHeader *)buffer_raw;
    buffer_hdr->begin = buffer_hdr->end = 0;
    buffer_ring = (Message *)((char *)buffer_raw + sizeof(BufferHeader));
}
void test_ipc(int parent_cpu, int child_cpu) {
 
    init_buffer(true); //init shared memory buffer
    //setting the third parameter of sem_init to non-zero creates a inter-process shared semaphore
    //semaphore's value starts with buffer_cap to allow sender to send buffer_cap messages at a time
    assert(sem_init(&(buffer_hdr->shm_semaphore_empty), 1, buffer_cap)==0);
    //semaphore's value starts with 0 to let receiver block waiting for sender's sends...
    assert(sem_init(&(buffer_hdr->shm_semaphore_nonempty), 1, 0) == 0);
 
    cpu_set_t set;
    CPU_ZERO(&set);

    if (fork() == 0) {
        // child process
        CPU_SET(child_cpu, &set);
        assert(sched_setaffinity(getpid(), sizeof(set), &set) == 0);
        sender();
        exit(0);
    } else {
        CPU_SET(parent_cpu, &set);
        assert(sched_setaffinity(getpid(), sizeof(set), &set) == 0);
        receiver();
        wait(NULL);
    }
   
}

static void* sender_start(void *arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(*(int *)arg, &set);
    sender();
    return NULL;
}

static void* receiver_start(void *arg) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(*(int *)arg, &set);
    receiver();
    return NULL;
}

void test_pthreads() {
    init_buffer(false); //init regular buffer
    //setting the third parameter of sem_init to zero creates a semaphore private to this process
    assert(sem_init(&(buffer_hdr->shm_semaphore_empty), 0, buffer_cap)==0);
    assert(sem_init(&(buffer_hdr->shm_semaphore_nonempty), 0, 0) == 0);
 
    pthread_t tids[2];
    assert(pthread_create(&tids[0], NULL, sender_start, &child_cpu) == 0);   
    assert(pthread_create(&tids[1], NULL, receiver_start, &parent_cpu) == 0);  
    for (int i = 0; i < 2; i++){
        pthread_join(tids[i], NULL);
    } 
}

int main(const int argc, const char** argv) {
    std::string mode = "ipc";
    if (argc > 1) {
        mode = argv[1];
    }
    if (argc > 2) {
        parent_cpu = atoi(argv[2]);
    }
    if (argc > 3) {
        child_cpu = atoi(argv[3]);
    }
    if (argc > 4) {
        buffer_cap = atoi(argv[4]);
    }
    std::cout << "Mode: " << mode << " Buffer capacity is: " << buffer_cap << std::endl; 


    if (mode == "ipc") {
        test_ipc(parent_cpu, child_cpu);
    }else if (mode == "threads") {
        test_pthreads();
    }else {
        std::cout <<"Unknown mode: " << mode << std::endl;
    }

}