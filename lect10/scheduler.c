#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define NUM_CHILDREN 10

#define POLICY_RR   0
#define POLICY_FCFS 1

#define STATE_READY   0
#define STATE_RUNNING 1
#define STATE_SLEEP   2
#define STATE_DONE    3

typedef struct {
    pid_t pid;
    int remaining_quantum;
    int status;
    int io_wait_time;      // tick 단위
    int total_wait_time;   // READY 대기 tick 누적

    int arrival_tick;
    int finish_tick;
    int enqueue_tick;      // FCFS READY 진입 시각

    // I/O 통계
    int io_count;          // I/O 요청 횟수
    int total_io_time;     // SLEEP에 머문 총 tick

    // [ADDED] Response time 통계
    int first_run_tick;    // 처음 RUNNING 된 tick (-1이면 아직)
} PCB;

typedef struct {
    int policy;
    int q;                 // RR이면 q, FCFS면 -1
    int total_ticks;
    double avg_wait;
    double avg_ta;
    double throughput;

    // I/O 평균 통계
    double avg_io_time;
    double avg_io_count;

    // [ADDED] 응답시간 평균
    double avg_response;

    // [ADDED] 문맥교환 횟수
    int context_switches;
} Result;

/* 전역 실험 - (1회마다 초기화) */
static PCB proc_table[NUM_CHILDREN];
static int current_proc_idx = -1;
static int total_done_cnt = 0;
static int now_tick = 0;

static int TIME_QUANTUM = 3;
static int policy = POLICY_RR;

/* [ADDED] context switch count */
static int context_switches = 0;

/* 핸들러 플래그 */
static volatile sig_atomic_t tick_flag = 0;
static volatile sig_atomic_t io_flag = 0;
static volatile sig_atomic_t chld_flag = 0;

/* 자식: tick 동기화 */
static volatile sig_atomic_t child_got_tick = 0;

static void child_tick_handler(int sig) {
    (void)sig;
    child_got_tick = 1;
}

/* Workload control */
static int g_fixed_workload = 1;          // 1=고정, 0=랜덤
static unsigned g_base_seed = 20251214u;  // 고정 시드

static void child_process_logic(int id) {
    if (g_fixed_workload) {
        srand(g_base_seed + (unsigned)id);
    } else {
        srand((unsigned)(time(NULL) ^ (getpid() << 16)));
    }

    int cpu_burst = (rand() % 10) + 1;
    printf("[Child %d] Created PID=%d, CPU_BURST=%d\n", id, (int)getpid(), cpu_burst);
    fflush(stdout);

    signal(SIGUSR1, child_tick_handler);

    sigset_t block_mask, oldmask;
    sigfillset(&block_mask);
    sigdelset(&block_mask, SIGUSR1);
    sigprocmask(SIG_SETMASK, &block_mask, &oldmask);

    while (1) {
        child_got_tick = 0;
        while (!child_got_tick) {
            sigsuspend(&oldmask);
        }

        cpu_burst--;

        if (cpu_burst <= 0) {
            if (rand() % 2 == 0) {
                printf("[Child %d] I/O Request!\n", id);
                fflush(stdout);
                kill(getppid(), SIGUSR2);
                cpu_burst = (rand() % 5) + 1;
            } else {
                printf("[Child %d] Finished. Exit.\n", id);
                fflush(stdout);
                _exit(0);
            }
        }
    }
}

static void alarm_handler(int sig) { (void)sig; tick_flag = 1; }
static void io_handler(int sig)    { (void)sig; io_flag = 1; }
static void chld_handler(int sig)  { (void)sig; chld_flag = 1; }

static const char* policy_name(int p) { return (p == POLICY_FCFS) ? "FCFS" : "RR"; }

/* I/O wait 가중치 */
static int draw_io_wait_weighted(void) {
    int r = rand() % 100;
    if (r < 40) return 1;
    if (r < 65) return 2;
    if (r < 85) return 3;
    if (r < 95) return 4;
    return 5;
}

/* 초기화 */
static void reset_globals(void) {
    for (int i = 0; i < NUM_CHILDREN; i++) {
        proc_table[i].pid = -1;
        proc_table[i].remaining_quantum = 0;
        proc_table[i].status = STATE_READY;
        proc_table[i].io_wait_time = 0;
        proc_table[i].total_wait_time = 0;
        proc_table[i].arrival_tick = 0;
        proc_table[i].finish_tick = -1;
        proc_table[i].enqueue_tick = 0;

        proc_table[i].io_count = 0;
        proc_table[i].total_io_time = 0;

        // [ADDED]
        proc_table[i].first_run_tick = -1;
    }
    current_proc_idx = -1;
    total_done_cnt = 0;
    now_tick = 0;

    tick_flag = 0;
    io_flag = 0;
    chld_flag = 0;

    // [ADDED]
    context_switches = 0;
}

/* 자식 수거 */
static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < NUM_CHILDREN; i++) {
            if (proc_table[i].pid == pid) {
                proc_table[i].status = STATE_DONE;
                proc_table[i].finish_tick = now_tick;
                total_done_cnt++;
                if (current_proc_idx == i) current_proc_idx = -1;
                break;
            }
        }
    }
}

/* I/O 요청 처리 */
static void handle_io_request(void) {
    if (current_proc_idx != -1 && proc_table[current_proc_idx].status == STATE_RUNNING) {
        int i = current_proc_idx;
        proc_table[i].status = STATE_SLEEP;

        proc_table[i].io_wait_time = draw_io_wait_weighted();
        proc_table[i].io_count++;

        current_proc_idx = -1;
    }
}

static int pick_next_ready_rr(void) {
    static int rr_cursor = 0;
    for (int k = 0; k < NUM_CHILDREN; k++) {
        int idx = (rr_cursor + k) % NUM_CHILDREN;
        if (proc_table[idx].status == STATE_READY && proc_table[idx].remaining_quantum > 0) {
            rr_cursor = (idx + 1) % NUM_CHILDREN;
            return idx;
        }
    }
    return -1;
}

static int pick_next_ready_fcfs(void) {
    int best = -1;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (proc_table[i].status == STATE_READY) {
            if (best == -1 || proc_table[i].enqueue_tick < proc_table[best].enqueue_tick) {
                best = i;
            }
        }
    }
    return best;
}

static void do_scheduler_tick(void) {
    now_tick++;

    /* READY 대기시간 누적 */
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (proc_table[i].status == STATE_READY) proc_table[i].total_wait_time++;
    }

    /* SLEEP 처리 */
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (proc_table[i].status == STATE_SLEEP) {
            proc_table[i].io_wait_time--;
            proc_table[i].total_io_time++;

            if (proc_table[i].io_wait_time <= 0) {
                proc_table[i].status = STATE_READY;
                proc_table[i].enqueue_tick = now_tick;
            }
        }
    }

    /* RUNNING 처리 */
    if (current_proc_idx != -1 && proc_table[current_proc_idx].status == STATE_RUNNING) {
        if (policy == POLICY_RR) proc_table[current_proc_idx].remaining_quantum--;
        kill(proc_table[current_proc_idx].pid, SIGUSR1);

        if (policy == POLICY_RR && proc_table[current_proc_idx].remaining_quantum <= 0) {
            proc_table[current_proc_idx].status = STATE_READY;
            proc_table[current_proc_idx].enqueue_tick = now_tick;
            current_proc_idx = -1;
        }
    }

    /* RR: 글로벌 퀀텀 리셋 */
    if (policy == POLICY_RR) {
        int any_left = 0;
        for (int i = 0; i < NUM_CHILDREN; i++) {
            if (proc_table[i].status != STATE_DONE && proc_table[i].remaining_quantum > 0) {
                any_left = 1;
                break;
            }
        }
        if (!any_left) {
            for (int i = 0; i < NUM_CHILDREN; i++) {
                if (proc_table[i].status != STATE_DONE) proc_table[i].remaining_quantum = TIME_QUANTUM;
            }
        }
    }

    /* CPU 비었으면 다음 선택 */
    if (current_proc_idx == -1) {
        int next = (policy == POLICY_FCFS) ? pick_next_ready_fcfs() : pick_next_ready_rr();
        if (next != -1) {
            // [ADDED] context switch count (첫 선택 포함)
            context_switches++;

            current_proc_idx = next;
            proc_table[current_proc_idx].status = STATE_RUNNING;

            // [ADDED] response time 기록(처음 한번만)
            if (proc_table[current_proc_idx].first_run_tick == -1) {
                proc_table[current_proc_idx].first_run_tick = now_tick;
            }
        }
    }
}

/* 결과 계산 */
static Result calc_result(int p, int q) {
    double sum_wait = 0.0, sum_ta = 0.0;
    double sum_io_time = 0.0, sum_io_count = 0.0;

    // [ADDED]
    double sum_resp = 0.0;

    for (int i = 0; i < NUM_CHILDREN; i++) {
        int ta = proc_table[i].finish_tick - proc_table[i].arrival_tick;
        sum_wait += proc_table[i].total_wait_time;
        sum_ta += ta;
        sum_io_time += proc_table[i].total_io_time;
        sum_io_count += proc_table[i].io_count;

        int rt = proc_table[i].first_run_tick - proc_table[i].arrival_tick;
        sum_resp += rt;
    }

    Result r;
    r.policy = p;
    r.q = q;
    r.total_ticks = now_tick;
    r.avg_wait = sum_wait / NUM_CHILDREN;
    r.avg_ta = sum_ta / NUM_CHILDREN;
    r.throughput = (now_tick > 0) ? (NUM_CHILDREN / (double)now_tick) : 0.0;

    r.avg_io_time = sum_io_time / NUM_CHILDREN;
    r.avg_io_count = sum_io_count / NUM_CHILDREN;

    // [ADDED]
    r.avg_response = sum_resp / NUM_CHILDREN;
    r.context_switches = context_switches;

    return r;
}

static void print_per_process_stats(void) {
    printf("\n==================== Per-Process Stats ====================\n");
    printf("+-----+--------+----------+----------+----------+---------+---------+\n");
    printf("| PID | WAIT   | RESP     | TA       | IO_CNT   | IO_TIME | FIN_T   |\n");
    printf("+-----+--------+----------+----------+----------+---------+---------+\n");
    for (int i = 0; i < NUM_CHILDREN; i++) {
        int rt = proc_table[i].first_run_tick - proc_table[i].arrival_tick;
        int ta = proc_table[i].finish_tick - proc_table[i].arrival_tick;
        printf("| P%-2d | %6d | %8d | %8d | %8d | %7d | %7d |\n",
               i,
               proc_table[i].total_wait_time,
               rt,
               ta,
               proc_table[i].io_count,
               proc_table[i].total_io_time,
               proc_table[i].finish_tick);
    }
    printf("+-----+--------+----------+----------+----------+---------+---------+\n");
}

static Result run_simulation(int p, int q, useconds_t tick_us, int progress_log, unsigned run_seed) {
    reset_globals();

    policy = p;
    TIME_QUANTUM = (q > 0) ? q : 3;

    srand(run_seed);

    signal(SIGALRM, alarm_handler);
    signal(SIGUSR2, io_handler);
    signal(SIGCHLD, chld_handler);

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            child_process_logic(i);
        } else if (pid > 0) {
            proc_table[i].pid = pid;
            proc_table[i].status = STATE_READY;
            proc_table[i].arrival_tick = now_tick;
            proc_table[i].finish_tick = -1;
            proc_table[i].enqueue_tick = now_tick;
            proc_table[i].first_run_tick = -1;

            if (p == POLICY_RR) proc_table[i].remaining_quantum = TIME_QUANTUM;
            else proc_table[i].remaining_quantum = 1;
        } else {
            perror("fork");
            exit(1);
        }
    }

    ualarm(tick_us, tick_us);

    while (total_done_cnt < NUM_CHILDREN) {
        pause();

        if (chld_flag) { chld_flag = 0; reap_children(); }
        if (io_flag)   { io_flag = 0; handle_io_request(); }
        if (tick_flag) { tick_flag = 0; do_scheduler_tick(); }
    }

    ualarm(0, 0);

    Result r = calc_result(p, (p == POLICY_RR) ? TIME_QUANTUM : -1);

    if (progress_log) {
        printf("[EXP] Finished %s q=%d | AVG_WAIT=%.2f AVG_RESP=%.2f AVG_TA=%.2f CTX=%d TH=%.4f ticks=%d\n",
               policy_name(p), r.q, r.avg_wait, r.avg_response, r.avg_ta,
               r.context_switches, r.throughput, r.total_ticks);
    }

    print_per_process_stats();

    return r;
}

/* repeat 평균 */
static Result run_repeat_avg(int p, int q, int repeat_n, useconds_t tick_us, int progress_log) {
    double w = 0.0, ta = 0.0, th = 0.0, ticks = 0.0;
    double io_time = 0.0, io_cnt = 0.0, resp = 0.0;
    double ctx = 0.0;

    for (int r = 0; r < repeat_n; r++) {
        unsigned seed = g_fixed_workload ? g_base_seed
                                         : ((unsigned)time(NULL) ^ (unsigned)(getpid() << 8) ^ (unsigned)r);

        Result one = run_simulation(p, q, tick_us, progress_log, seed);

        w += one.avg_wait;
        resp += one.avg_response;
        ta += one.avg_ta;
        th += one.throughput;
        ticks += one.total_ticks;

        io_time += one.avg_io_time;
        io_cnt += one.avg_io_count;

        ctx += one.context_switches;
    }

    Result avg;
    avg.policy = p;
    avg.q = (p == POLICY_RR) ? q : -1;
    avg.avg_wait = w / repeat_n;
    avg.avg_response = resp / repeat_n;
    avg.avg_ta = ta / repeat_n;
    avg.throughput = th / repeat_n;
    avg.total_ticks = (int)(ticks / repeat_n);

    avg.avg_io_time = io_time / repeat_n;
    avg.avg_io_count = io_cnt / repeat_n;

    avg.context_switches = (int)(ctx / repeat_n);
    return avg;
}

static void print_table(Result *arr, int n, int repeat_n) {
    printf("\n==================== Summary Table (repeat=%d, fixed=%d, seed=%u) ====================\n",
           repeat_n, g_fixed_workload, g_base_seed);
    printf("+--------+------+----------+----------+----------+------------+-----------+-----------+-------+\n");
    printf("| Policy |  q   | AVG_WAIT | AVG_RESP |  AVG_TA  | THROUGHPUT | AVG_IO_T  | AVG_IO_C  | CTX   |\n");
    printf("+--------+------+----------+----------+----------+------------+-----------+-----------+-------+\n");
    for (int i = 0; i < n; i++) {
        const char *pn = policy_name(arr[i].policy);
        if (arr[i].q < 0)
            printf("| %-6s |  -   | %8.2f | %8.2f | %8.2f | %10.4f | %9.2f | %9.2f | %5d |\n",
                   pn, arr[i].avg_wait, arr[i].avg_response, arr[i].avg_ta, arr[i].throughput,
                   arr[i].avg_io_time, arr[i].avg_io_count, arr[i].context_switches);
        else
            printf("| %-6s | %4d | %8.2f | %8.2f | %8.2f | %10.4f | %9.2f | %9.2f | %5d |\n",
                   pn, arr[i].q, arr[i].avg_wait, arr[i].avg_response, arr[i].avg_ta, arr[i].throughput,
                   arr[i].avg_io_time, arr[i].avg_io_count, arr[i].context_switches);
    }
    printf("+--------+------+----------+----------+----------+------------+-----------+-----------+-------+\n");
    printf("=====================================================================================\n\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int q_list[] = {1, 3, 5, 8, 12};
    int q_n = (int)(sizeof(q_list) / sizeof(q_list[0]));

    useconds_t tick_us = 100000;   // 0.1초 tick
    int progress_log = 0;
    int repeat_n = 1;

    g_fixed_workload = 1;
    g_base_seed = 20251214u;

    Result results[64];
    int rcnt = 0;

    for (int i = 0; i < q_n; i++) {
        results[rcnt++] = run_repeat_avg(POLICY_RR, q_list[i], repeat_n, tick_us, progress_log);
    }

    results[rcnt++] = run_repeat_avg(POLICY_FCFS, -1, repeat_n, tick_us, progress_log);

    print_table(results, rcnt, repeat_n);

    return 0;
}

