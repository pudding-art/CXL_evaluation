// SPDX-License-Identifier: LGPL-2.1-only

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>
#include <ctype.h>
#include <numa.h>
#include <time.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/signalfd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <fnmatch.h>
#include <pty.h>

#define KB 1024
#define MB (1024*1024)
#define GB (1024*1024*1024)

#define TIMER_SIGNAL SIGRTMIN

#define PERR(cond, msg) do {if (cond) {perror(msg); exit(EXIT_FAILURE);}} while (0)

static char tmp[4096];

float get_time()
{
    static struct timespec start = {};

    if (start.tv_sec == 0 && start.tv_nsec == 0) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        return 0.0f;
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
	    return (now.tv_sec-start.tv_sec) + 1e-9*(now.tv_nsec-start.tv_nsec);
    }
}

// Parts borrowed from numastat (GPL)
// 计算当前进程每个node的内存分配情况——node
void emu_show_stats(int pid)
{
    char fname[64];
    snprintf(fname, sizeof(fname), "/proc/%d/numa_maps", pid);

    struct timespec time = {};
    clock_gettime(CLOCK_MONOTONIC, &time);

    FILE *fs = fopen(fname, "r");
    if (!fs) {
        sprintf(tmp, "emu: show_stats: can't read %s", fname);
        perror(tmp);
        exit(EXIT_FAILURE);
    }

   // long long node_pages[2] = {}; // node0 + node1
    long long node_pages[3] = {}; // add CXL node

    // Parse numa_maps
    while (fgets(tmp, sizeof(tmp), fs)) {
        const char *delimiters = " \t\r\n";
        char *p = strtok(tmp, delimiters); // 分解/proc/<pid>/numa_maps中的内容
    //00400000 defaultfile=/usr/local/bin/app mapped=1 active=0 N3=1 kernelpagesize_kB=4
    //应该会打印出以上的内容
        while (p) { //解析每段内容
            if (p[0] == 'N') { //找到和numa node相关的内容
                int node = (int)strtol(&p[1], &p, 10); // int node = (int)strtol(&p[1], &p, 10); 将第二个字符转换为int
                if (node != 0 && node != 1 && node != 2) { //add node2
                    fprintf(stderr, "emu: warning: skipping data for node %d\n", node);
                }
                if (p[0] != '=') {//一般都是Nx=num. 如果第三位不是=则说明当前解析出错
                    fprintf(stderr, "emu: show_stats: node value parse error");
                    exit(EXIT_FAILURE);
                }
                long pages = (double)strtol(&p[1], &p, 10);
                node_pages[node] += pages;
            } else if (strncmp(p, "huge", 4) == 0) {
                fprintf(stderr, "emu: warning: skipping huge pages, not supported (yet)\n");
            }

            p = strtok(NULL, delimiters);//继续使用之前的函进行分割,返回下一个标记,并将个指向该标记的指针赋值给p
        }
    }

    //long long total = node_pages[0] + node_pages[1];
    long long total = node_pages[0] + node_pages[1] + node_pages[2];
    // old version: float local_frac is node0 / total
    // float local_frac = node_pages[0] / (float)total;
    // new verison: local node is changed to node1, cause cxl node is mount at node1
    float local_frac = node_pages[1] / (float)total;
    // printf("emu: local_fraction:%% %3.2f localGB: %.2f remoteGB %.2f totalGB %.2f time %.2f\n",
    //         100.0f * local_frac,
    //         numa_pagesize()*node_pages[0]/(float)GB,
    //         numa_pagesize()*node_pages[1]/(float)GB,
    //         numa_pagesize()*total/(float)GB,
	//         get_time());
     printf("emu: local_fraction:%% %3.2f localGB: %.2f remoteGB: %.2f cxlGB: %.2f totalGB: %.2f time: %.2f\n",
            100.0f * local_frac,
            numa_pagesize()*node_pages[1]/(float)GB,
            numa_pagesize()*node_pages[0]/(float)GB,
            numa_pagesize()*node_pages[2]/(float)GB,
            numa_pagesize()*total/(float)GB,
	        get_time());

    if (fclose(fs)) {
        sprintf(tmp, "emu: can't close '%s'", fname);
        perror(tmp);
        exit(EXIT_FAILURE);
    }
}

void memprof_clear_refs(int pid)
{
    char fname[64];
    snprintf(fname, sizeof(fname), "/proc/%d/clear_refs", pid);
    FILE *fp = fopen(fname, "w");
    if (!fp) {
        sprintf(tmp, "memprof: can't open '%s'", fname);
        perror(tmp);
        exit(EXIT_FAILURE);
    }
    const char one[] = "1\n";
    if (1 != fwrite(one, sizeof(one), 1, fp)) {
        sprintf(tmp, "memprof: can't write to '%s'", fname);
        perror(tmp);
        exit(EXIT_FAILURE);
    }
    if (fclose(fp)) {
        sprintf(tmp, "memprof: can't close '%s'", fname);
        perror(tmp);
        exit(EXIT_FAILURE);
    }
}


// 通过smaps_rollup文件查找有关当前进程内存使用情况的详细信息——process
void memprof_show_stats(int pid)
{
    char fname[64];
    snprintf(fname, sizeof(fname), "/proc/%d/smaps_rollup", pid);
    FILE *fp = fopen(fname, "r");
    if (!fp) {
        sprintf(tmp, "memprof: can't open '%s'", fname);
        perror(tmp);
        exit(EXIT_FAILURE);
    }

    size_t rss = 0, pss = 0, ref = 0, anon = 0;

    while (fgets(tmp, sizeof(tmp), fp)) {
        sscanf(tmp, "Rss: %zu kB", &rss); /// 当前驻留在RAM中的映射的量
        sscanf(tmp, "Pss: %zu kB", &pss); // 进程对此映射的比例份额
        sscanf(tmp, "Referenced: %zu kB", &ref); // 指示当前标记为已引用或已访问的内存量
        sscanf(tmp, "Anonymous: %zu kB", &anon); // 显式不属于任何文件的内存量，即使与文件关联的映射可能包含匿名页面
    }

    float hot = 0;
    if (rss)
        hot = (float)ref / (float)rss;  // ref是已经被进程访问的页面数，rss是实际占用的页面数，可以计算进程实际物理内存占用中被访问的比例
    float time = get_time();
    printf("memprof: rssKB %zu pssKB %zu refKB %zu hot %.4f time %.2f anonKB %zu\n",
            rss, pss, ref, hot, time, anon);

    if (fclose(fp)) {
        sprintf(tmp, "memprof: can't close '%s'", fname);
        perror(tmp);
        exit(EXIT_FAILURE);
    }
}

void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-l size] [-i] PROG [ARGS ...]\n", argv0);
}

int main(int argc, char **argv)
{
    // initalize
    get_time();

    int opt;
    int enable_emu = 1;
    int enable_memprof = 0;
    int rank = 0;
    struct timespec interval = {};
    interval.tv_sec = 1;
    const char *start_pattern = NULL;
    const char *end_pattern = NULL;

    long long emu_local_size = -1; //lock的本地内存大小，来自-l参数
    int emu_interleave = 0; // -i enable interleave policy

    while ((opt = getopt(argc, argv, "+l:in:t:mS:E:")) != -1) {
        switch (opt) { //检查输入输出参数是否正确，并将信息保留
        case 'l': //控制本地内存lock大小
        {
            char *end = NULL;
            double size = strtod(optarg, &end);

            if (end == optarg) {
                fprintf(stderr, "error: invalid size in -l option\n");
                usage(argv[0]);
                exit(EXIT_FAILURE);
            }

            switch (tolower(*end)) {
            case 'g':
                emu_local_size = size * GB;
                break;
            case 'm':
                emu_local_size = size * MB;
                break;
            case 'k':
                emu_local_size = size * KB;
                break;
            case '\0':
                emu_local_size = size;
                break;
            default:
                fprintf(stderr, "error: invalid size in -l option\n");
                usage(argv[0]);
                exit(EXIT_FAILURE);
            }
        } break;
        case 'i':
            emu_interleave = 1;
            break;
        case 'n':
            rank = atoi(optarg);
            break;
        case 't':
            interval.tv_sec = atoi(optarg);
            break;
        case 'm':
            enable_emu = 0;
            enable_memprof = 1;
            break;
        case 'S':
            start_pattern = optarg;
            break;
        case 'E':
            end_pattern = optarg;
            break;
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    if (argc - optind < 1) {
        usage(argv[0]);
        return 1;
    }

    bool state_monitoring = true;
    if (start_pattern)
        state_monitoring = false;

    const bool monitor_out = start_pattern || end_pattern;

    // Ensure output is interleaved with target app
    if (rank == 0)
        setlinebuf(stdout);

    // Block signals we will wait on with epoll
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, TIMER_SIGNAL);
    sigaddset(&sigset, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &sigset, NULL) < 0) {
        fprintf(stderr, "emu: error: sigprocmask\n");
        exit(EXIT_FAILURE);
    }
    int sigfd = signalfd(-1, &sigset, 0);
    PERR(sigfd < 0, "emu: signalfd");

    bool enable_timer = interval.tv_sec != 0;
    timer_t timerid;
    struct itimerspec timerspec = {};

    if (enable_timer) {
        struct sigevent timer_event = {};
        timer_event.sigev_notify = SIGEV_SIGNAL; //通知方式
        timer_event.sigev_signo = TIMER_SIGNAL; //通知信号
        PERR(timer_create(CLOCK_MONOTONIC, &timer_event, &timerid) < 0, "emu: timer_create");

        timerspec.it_interval = interval;
        timerspec.it_value = interval;

        if (state_monitoring) {
            PERR(timer_settime(timerid, 0, &timerspec, NULL) < 0, "emu: arm timer");
        }
    }

    void *emu_dummyptr = NULL;
    long long emu_dummysize = 0;

    if (enable_emu) {
        long long free0; // available memory capacity on node0
        long long free1; // available memory capacity on node1
        // numa_available() && numa_num_task_nodes() > 2

        // numa_set_membind(3)
        
        // old version  //绑定进程在node0上运行
        //numa_run_on_node(0);
        numa_run_on_node(1);//改成绑定在node1上运行n

        // Fail allocation if requested node is full
        numa_set_strict(1);

        // Fill up local NUMA node to requested size
        if (rank == 0 && emu_local_size > 0) {
            // long long numa_node_size(int node, long long *freep);
             //返回一个节点的内存大小，返回参数是内存的总大小，这些内存不一定都是程序可用的
            //numa_node_size(0, &free0);
            numa_node_size(1, &free1);
            printf("emu: availGB %.2f\n", free1/(float)GB);//第二个参数是一个指针，可以用节点上的空闲内存填充它

            if (free1 > emu_local_size) {//现在节点的空闲内存大于用户想要lock的内存大小，正确情况
                emu_dummysize = free1 - emu_local_size;
                printf("emu: allocatingGB %.2f\n", emu_dummysize/(float)GB);//剩余可分配内存大小

                // 分配大小 分配在哪个节点上
                //emu_dummyptr = numa_alloc_onnode(emu_dummysize, 0);//在特定节点上分配memory, emu_dummysize < total memory size
                emu_dummyptr = numa_alloc_onnode(emu_dummysize, 1); //分配在node1上
                //memset(dummyptr, 0xff, dummysize);

                // Fault and prevent swapping
                PERR(mlock(emu_dummyptr, emu_dummysize) < 0, "emu: mlock, is the memlock rlimit too low?");
            } else if (free1 < emu_local_size) {
                fprintf(stderr, "error: only %lld bytes free on node 0\n", free0);
                return 1;
            }
        }

        if (rank == 0) {
            numa_node_size(0, &free1);
            printf("emu: availGB %.2f\n", free1/(float)GB);
        }
    }

    // Start target app
    int target_pipe[2] = {-1, -1};

    if (monitor_out) {
        //PERR(pipe(target_pipe) < 0, "emu: failed pipe()");
	
        // Use pty to get unbuffered output. Borrowed from slurm (GPL)
        PERR(openpty(target_pipe, target_pipe + 1, NULL, NULL, NULL) < 0, "emu: openpty");
        struct termios tio;
        memset(&tio, 0, sizeof(tio));
        if (tcgetattr(target_pipe[1], &tio) == 0) {
            tio.c_oflag &= ~OPOST;
            if (tcsetattr(target_pipe[1], 0, &tio) != 0) {
                perror("emu: tcsetattr");
            }
        }
    }

    int pid = fork();
    PERR(pid < 0, "emu: fork");//创建子进程失败

    if (pid == 0) {//父进程对应的操作
        if (enable_emu) {
            if (emu_interleave) {//开启interleave
            //但是system中现在有3个nodes，这里只想在其中的两个nodes上进行节点交错，怎么改在node1和node2上interleave?
            //但是这个不能作为容量扩展的内容
                struct bitmask *nodemask = numa_allocate_nodemask();
                if(nodemask == NULL){
                    printf("interleave nodemask encounters failures.");
                    return -1;
                }
                numa_bitmask_setbit(nodemask, 1);//set node1
                numa_bitmask_setbit(nodemask, 2);//set node2
                //numa_set_interleave_mask(numa_all_nodes_ptr); 
                // 允许对当前线程进行交错，所有将来的内存分配都是通过交错指定的节点请求来分配内存的
                numa_set_interleave_mask(nodemask);//在node1和node2上交错分配内存

                //检测交错分配内存是否成功
                struct bitmask *temp = numa_get_interleave_mask();
                if (temp == NULL){
                    perror("numa get interleave mask is nothing!");
                    return -1;
                }
            }

            if (emu_local_size == 0) {
                struct bitmask *mask = numa_parse_nodestring("1");
                numa_set_membind(mask);
                /*
            } else if (emu_local_size < 0) {
                printf("emu: binding to local memory\n");
                struct bitmask *mask = numa_parse_nodestring("0");
                numa_set_membind(mask);
                */
            }
        }

        if (monitor_out) {
            PERR(dup2(target_pipe[1], STDOUT_FILENO) < 0, "emu: child dup2");

            close(target_pipe[1]);
            close(target_pipe[0]);
        }

        if (execvp(argv[optind], argv + optind) < 0) {  //真正执行application的代码，失败返回-1
            sprintf(tmp, "emu: exec target '%s'", argv[optind]);
            perror(tmp);
            exit(EXIT_FAILURE);
        }
    }

    int outfd = -1;
    FILE *outstream = NULL;

    if (monitor_out) {
        close(target_pipe[1]);
        outfd = target_pipe[0];

        int flags = 0;
        flags = fcntl(outfd, F_GETFL);
        PERR(flags < 0, "emu: get target stdout flags");

        flags = fcntl(outfd, F_SETFL, flags | O_NONBLOCK);
        PERR(flags < 0, "emu: set target stdout non-blocking");

        outstream = fdopen(outfd, "r");
        PERR(!outstream, "emu: fdopen target stdout");
    }

    // Use epoll to wait for the following events:
    // - timer fired
    // - target exited
    // - target wrote to stdout (if monitoring)

    int epfd = epoll_create1(0);//创建一个epoll实例
    PERR(epfd < 0, "emu: epoll_create");

    struct epoll_event ep_event = {}; //epoll事件列表
    ep_event.events = EPOLLIN; //add的事件

    if (monitor_out) {
        ep_event.data.fd = outfd;
        PERR(epoll_ctl(epfd, EPOLL_CTL_ADD, outfd, &ep_event) < 0,
            "emu: add target stdout to epoll");
    }

    ep_event.data.fd = sigfd;
    PERR(epoll_ctl(epfd, EPOLL_CTL_ADD, sigfd, &ep_event) < 0,
            "emu: add signalfd to epoll");

    for (;;) {
        PERR(epoll_wait(epfd, &ep_event, 1, -1) < 0, "emu: epoll_wait");//等待事件发生

        if (ep_event.data.fd == sigfd) {
            struct signalfd_siginfo info;
            PERR(read(sigfd, &info, sizeof(info)) != sizeof(info), "emu: read siginfo failed");

            if (info.ssi_signo == SIGCHLD) {
                if (info.ssi_pid != pid)
                    continue;

                if (info.ssi_code == CLD_CONTINUED)
                    continue;
                else if (info.ssi_code == CLD_STOPPED) {
                    printf("emu: stop\n");

                    if (enable_memprof) {
                        if (rank == 0) {
                            memprof_show_stats(pid);
                            memprof_clear_refs(pid);
                        }
                    }

                    printf("emu: continue\n");
                    PERR(kill(pid, SIGCONT) < 0, "emu: send SIGCONT");
                } else
                    break;
            } else if (info.ssi_signo == TIMER_SIGNAL) {
                if (enable_emu) {
                    emu_show_stats(pid);
                }

                if (enable_memprof) {
                    if (rank == 0) {
                        memprof_show_stats(pid);
                        memprof_clear_refs(pid);
                    }
                }
            } else {
                fprintf(stderr, "emu: unexpected signal %d\n", info.ssi_signo);
            }
        } else if (ep_event.data.fd == outfd) {
            errno = 0;
            while (fgets(tmp, sizeof(tmp), outstream)) {
                printf("%s", tmp);

                // Remove newline for pattern matching
                for (char *t = tmp; *t; t++) {
                    if (*t == '\n') {
                        *t = '\0';
                        break;
                    }
                }

                if (!state_monitoring && start_pattern &&
                        fnmatch(start_pattern, tmp, 0) == 0)
                {
                    state_monitoring = true;
                    printf("emu: start %.2f\n", get_time());

                    if (enable_emu)
                        emu_show_stats(pid);

                    if (enable_memprof && rank == 0)
                        memprof_clear_refs(pid);

                    if (enable_timer) {
                        PERR(timer_settime(timerid, 0, &timerspec, NULL) < 0, "emu: timer_settime");
                    }
                } else if (state_monitoring && end_pattern &&
                        fnmatch(end_pattern, tmp, 0) == 0)
                {
                    if (enable_emu)
                        emu_show_stats(pid);

                    // If timer is enabled, then printing stats here would be
                    // confusing since the last interval would be shorter
                    // then all the others.
                    if (enable_memprof && rank == 0 && !enable_timer)
                        memprof_show_stats(pid);

                    if (enable_timer) {
                        struct itimerspec zero = {};
                        PERR(timer_settime(timerid, 0, &zero, NULL) < 0, "emu: disarm timer");
                    }

                    state_monitoring = false;
                    printf("emu: end %.2f\n", get_time());
                }
            }

            // fgets result
            PERR(errno && errno != EAGAIN, "emu: read target stdout");
        } else {
            fprintf(stderr, "emu: unexpected epoll_wait fd %d\n", ep_event.data.fd);
            exit(EXIT_FAILURE);
        }
    }

    if (emu_dummyptr) {
        numa_free(emu_dummyptr, emu_dummysize);
    }
}
