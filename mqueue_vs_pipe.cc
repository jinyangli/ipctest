#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <mqueue.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <assert.h>

#include <string>
#include <iostream>
#include <vector>
#include <cstdint>
#include <algorithm>

extern int errno;

const int num_repeat = 2000;
int wait_period = 100;

// Message used in our system
struct Message {
    int32_t cmd;
    int model_id;
    int stage_id;
    int batch_size;
    float deadline;
    timespec ts;
};

// for message queue send
const int priority = 1;

inline int64_t time_elapsed(const timespec& start, const timespec& end) {
    return (end.tv_sec - start.tv_sec) * 1000000000 + (end.tv_nsec - start.tv_nsec);
}

void process_data(const std::vector<Message>& messages, const std::vector<timespec>& recv_time, const std::string& msg) {
    std::vector<uint64_t> data;
    assert(messages.size() == num_repeat && recv_time.size() == num_repeat);
    for (int i = num_repeat / 2; i < num_repeat; ++i) {
        int64_t t = time_elapsed(messages[i].ts, recv_time[i]);
        assert(t > 0);
        data.push_back(t);
    }
    const int num = data.size();
    assert(num > 0);
    double sum = 0;
    for (const auto& x : data) sum += x;
    double mean = sum / num;
    sum = 0;
    for (const auto& x : data) sum += (x - mean) * (x - mean);
    double var = std::sqrt(sum / num);
    std::sort(data.begin(), data.end());
    std::cout << msg << " " << wait_period << " " << mean << " " << var << " " << data[num / 2] << " " << data[num * 4 / 5] << std::endl;
}

inline void wait_for(int wait_period) {
    //usleep(wait_period);
    int64_t wait_period_in_ns = wait_period * 1000;
    timespec start, end;
    assert(clock_gettime(CLOCK_REALTIME, &start) == 0);
    while (true) {
        assert(clock_gettime(CLOCK_REALTIME, &end) == 0);
        if (time_elapsed(start, end) >= wait_period_in_ns) break;
    }
}

#define SERVER_TO_WORKER "/server_to_worker"
#define WORKER_TO_SERVER "/worker_to_server"
#define MODE 0644

void mq_child() {
    // Open server_to_worker mqueue for read
    mqd_t from_server = mq_open(SERVER_TO_WORKER, O_RDONLY); 
    if (from_server == -1) {
        perror("mq_open failure on server_to_worker");
        exit(-1);
    };

    // Open worker_to_server mqueue for write
    mq_attr attr;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Message);
    attr.mq_flags = 0;
    mqd_t to_server = mq_open(WORKER_TO_SERVER, O_CREAT | O_WRONLY, MODE, &attr);
    if (to_server == -1) {
        perror("mq_open failure on worker_to_server");
        exit(-1);
    };
     
    std::vector<Message> messages(num_repeat);
    std::vector<timespec> recv_time(num_repeat);

    // Signal server setup has finished
    Message msg;
    assert(mq_send(to_server, reinterpret_cast<char*>(&msg), sizeof(Message), priority) == 0);

    // Start profiling
    for (int i = 0; i < num_repeat; ++i) {
        assert(mq_receive(from_server, reinterpret_cast<char*>(&(messages[i])), sizeof(Message), NULL) == sizeof(Message));
        assert(clock_gettime(CLOCK_REALTIME, &(recv_time[i])) == 0);
    }

    process_data(messages, recv_time, "mqueue");

    if (mq_close(from_server) == -1) {
        perror("mq_close failure on mqfd");
        exit(-1);
    }
    if (mq_close(to_server) == -1) {
        perror("mq_close failure on mqfd");
        exit(-1);
    }
}

void mq_server(mqd_t to_child) {
    // Open worker_to_worker queue for read
    mq_attr attr;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Message);
    attr.mq_flags = 0;
    mqd_t from_child = mq_open(WORKER_TO_SERVER, O_CREAT | O_RDONLY, MODE, &attr);
    if (from_child == -1) {
        perror("mq_open failure from mq_server");
        exit(-1);
    };

    // Wait for worker to finish setup
    Message msg;
    assert(mq_receive(from_child, reinterpret_cast<char*>(&msg), sizeof(Message), NULL) == sizeof(Message));

    // Start profiling
    for (int i = 0; i < num_repeat; ++i) {
        wait_for(wait_period);
        assert(clock_gettime(CLOCK_REALTIME, &msg.ts) == 0);
        assert(mq_send(to_child, reinterpret_cast<char*>(&msg), sizeof(Message), priority) == 0);
    }

    // clean up
    assert(mq_close(to_child) == 0);
    assert(mq_close(from_child) == 0);
    assert(mq_unlink(SERVER_TO_WORKER) == 0);
    assert(mq_unlink(WORKER_TO_SERVER) == 0);
}

void test_mqueue() {
    //std::cout << "Start to test mqueue!" << std::endl;
    //std::cout << "Wait period " << wait_period << std::endl;
    // Open server_to_worker queue for write
    // We could open after fork, but just to match pipe's style
    mq_attr attr;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Message);
    attr.mq_flags = 0;
    mqd_t mqfd = mq_open(SERVER_TO_WORKER, O_CREAT | O_WRONLY, MODE, &attr);
    if (mqfd == -1) {
        perror("mq_open failure in test_mqueue");
        exit(-1);
    };
    if (fork() == 0) {
        // child process
        // close the queue inherited from parent because it's Write-only, will
        // later reopen for Read-only
        if (mq_close(mqfd) == -1) {
            perror("mq_close failure on mqfd");
            exit(-1);
        }
        mq_child();
        exit(0);
    } else {
        mq_server(mqfd);
        wait(NULL);
    }
}

void pipe_worker(int read_end, int write_end) {
    std::vector<Message> messages(num_repeat);
    std::vector<timespec> recv_time(num_repeat);
    // signal server setup has finished 
    Message msg;
    assert(write(write_end, &msg, sizeof(Message)) == sizeof(Message));
    // start profiling
    for (int i = 0; i < num_repeat; ++i) {
        assert(read(read_end, reinterpret_cast<char*>(&(messages[i])), sizeof(Message)) == sizeof(Message));
        assert(clock_gettime(CLOCK_REALTIME, &(recv_time[i])) == 0);
    }
    process_data(messages, recv_time, "pipe");
}

void pipe_server(int read_end, int write_end) {
    Message msg;
    // wait for worker to finish setup
    assert(read(read_end, reinterpret_cast<char*>(&msg), sizeof(Message)) == sizeof(Message));
    // start profiling
    for (int i = 0; i < num_repeat; ++i) {
        wait_for(wait_period);
        assert(clock_gettime(CLOCK_REALTIME, &msg.ts) == 0);
        assert(write(write_end, reinterpret_cast<char*>(&msg), sizeof(Message)) == sizeof(Message));
    }
}

void test_pipe() {
    //std::cout << "Start to test pipe!" << std::endl;
    //std::cout << "Wait period " << wait_period << std::endl;
    int server2worker[2];
    int status = pipe(server2worker);
    assert(status != -1);
    int worker2server[2];
    status = pipe(worker2server);
    assert(status != -1);
    if (fork() == 0) {
        // child process
        // close the ends that should be used by parent process
        // 0 for read end, 1 for write end
        close(server2worker[1]);
        close(worker2server[0]);
        pipe_worker(server2worker[0], worker2server[1]);
        // close pipes
        close(server2worker[0]);
        close(worker2server[1]);
        // child process exits here
        exit(0);
    } else {
        // parent process
        // close the ends that should be used by child process
        close(server2worker[0]);
        close(worker2server[1]);
        pipe_server(worker2server[0], server2worker[1]);
        close(server2worker[1]);
        close(worker2server[0]);
        wait(NULL);
    }
}

void print_usage() {
    std::cout << "Usage: ./main pipe|mqueue [wait_period]" << std::endl;
}

int main(const int argc, const char** argv) {
    if (argc != 2 && argc != 3) {
        print_usage();
        exit(-1);
    }
    std::string mode = argv[1];
    if (argc == 3) {
        wait_period = std::atoi(argv[2]);
    }
    if (mode == "pipe") {
        test_pipe();
    } else if (mode == "mqueue") {
        test_mqueue();
    } else {
        print_usage();
        exit(-1);
    }
    return 0;
}
