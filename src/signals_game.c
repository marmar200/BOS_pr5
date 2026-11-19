#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROUNDS 10

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t event_ready = 0;
static volatile sig_atomic_t event_signo = 0;
static volatile sig_atomic_t event_value = 0;
static volatile sig_atomic_t rt_signal = 0;
static volatile sig_atomic_t child_pid = 0;

static void handle_game(int signo, siginfo_t *info, void *context)
{
    (void)context;
    event_signo = signo;
    if (info != NULL && signo == rt_signal) {
        event_value = info->si_value.sival_int;
    }
    if (signo == SIGINT || signo == SIGTERM) {
        stop_requested = 1;
        if (child_pid > 0) {
            kill((pid_t)child_pid, SIGTERM);
        }
    }
    event_ready = 1;
}

static void handle_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
    if (child_pid > 0) {
        kill((pid_t)child_pid, SIGTERM);
    }
}

static int install_game_handler(int signo)
{
    struct sigaction action;

    memset(&action, 0, sizeof action);
    action.sa_sigaction = handle_game;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR1);
    sigaddset(&action.sa_mask, SIGUSR2);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    sigaddset(&action.sa_mask, (int)rt_signal);
    return sigaction(signo, &action, NULL);
}

static int wait_for_signal(const sigset_t *suspend_mask, int *value)
{
    while (event_ready == 0 && stop_requested == 0) {
        sigsuspend(suspend_mask);
    }
    if (stop_requested) {
        return -1;
    }

    if (value != NULL) {
        *value = (int)event_value;
    }
    event_ready = 0;
    return (int)event_signo;
}

static int send_guess(pid_t target, int guess)
{
    union sigval value;

    if (stop_requested) {
        return -1;
    }
    value.sival_int = guess;
    if (sigqueue(target, (int)rt_signal, value) != 0) {
        return -1;
    }
    return 0;
}

static int send_result(pid_t target, int signo)
{
    if (stop_requested) {
        return -1;
    }
    if (kill(target, signo) != 0) {
        return -1;
    }
    return 0;
}

static int think_round(pid_t other, int upper, int round, int thinker_id, const sigset_t *suspend_mask)
{
    int secret = 1 + (int)(random() % upper);
    int value = 0;

    printf("Раунд %d. Загадывает игрок %d.\n", round, thinker_id);
    fflush(stdout);

    if (send_guess(other, 0) != 0) {
        return -1;
    }

    for (;;) {
        int signo = wait_for_signal(suspend_mask, &value);
        if (signo < 0) {
            return -1;
        }
        if (signo != (int)rt_signal || value < 1 || value > upper) {
            return -1;
        }
        if (value == secret) {
            return send_result(other, SIGUSR1);
        }
        if (send_result(other, SIGUSR2) != 0) {
            return -1;
        }
    }
}

static int guess_round(pid_t other, int upper, int round, int guesser_id, const sigset_t *suspend_mask)
{
    struct timespec start;
    struct timespec finish;
    int value = 0;
    int signo;
    int guess;
    int attempts = 0;

    (void)round;
    signo = wait_for_signal(suspend_mask, &value);
    if (signo < 0 || signo != (int)rt_signal || value != 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (guess = 1; guess <= upper; guess++) {
        attempts++;
        if (send_guess(other, guess) != 0) {
            return -1;
        }
        signo = wait_for_signal(suspend_mask, &value);
        if (signo < 0) {
            return -1;
        }
        if (signo == SIGUSR1) {
            clock_gettime(CLOCK_MONOTONIC, &finish);
            printf("Игрок %d: попытка %d (число %d) — угадал\n", guesser_id, attempts, guess);
            printf("Игрок %d угадал число %d за %d попыток, время %.3f с\n",
                   guesser_id, guess, attempts,
                   (finish.tv_sec - start.tv_sec) + (finish.tv_nsec - start.tv_nsec) / 1000000000.0);
            fflush(stdout);
            return 0;
        }
        if (signo != SIGUSR2) {
            return -1;
        }
        printf("Игрок %d: попытка %d (число %d) — не угадал\n", guesser_id, attempts, guess);
        fflush(stdout);
    }

    fprintf(stderr, "pr5: число не найдено\n");
    return -1;
}

static int play_all_rounds(int is_parent, pid_t other, int upper, const sigset_t *suspend_mask)
{
    int my_id = is_parent ? 1 : 2;
    int round;

    for (round = 1; round <= ROUNDS; round++) {
        int thinker_is_parent = (round % 2 == 1);
        int i_think = is_parent == thinker_is_parent;

        if (stop_requested) {
            return -1;
        }
        if (i_think) {
            if (think_round(other, upper, round, my_id, suspend_mask) != 0) {
                return -1;
            }
        } else if (guess_round(other, upper, round, my_id, suspend_mask) != 0) {
            return -1;
        }
    }

    return 0;
}

static int read_upper(int argc, char *argv[])
{
    if (argc >= 2) {
        char *end = NULL;
        long value = strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || value < 1 || value > 100000) {
            return -1;
        }
        return (int)value;
    }

    printf("Введите N (верхняя граница, от 1 до 100000): ");
    fflush(stdout);
    {
        int value = 0;
        if (scanf("%d", &value) != 1 || value < 1 || value > 100000) {
            return -1;
        }
        return value;
    }
}

static void finish_parent(void)
{
    int status;

    if (child_pid <= 0) {
        return;
    }
    kill((pid_t)child_pid, SIGTERM);
    while (waitpid((pid_t)child_pid, &status, 0) < 0 && errno == EINTR) {
    }
    child_pid = 0;
}

int main(int argc, char *argv[])
{
    int upper = read_upper(argc, argv);
    sigset_t blocked;
    sigset_t suspend_mask;
    struct sigaction stop_action;
    pid_t pid;

    if (upper < 0) {
        fprintf(stderr, "Использование: %s [N]\nN — целое от 1 до 100000.\n", argv[0]);
        return 1;
    }

    rt_signal = SIGRTMIN;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, (int)rt_signal);
    if (sigprocmask(SIG_BLOCK, &blocked, &suspend_mask) != 0) {
        perror("sigprocmask");
        return 1;
    }
    if (install_game_handler(SIGUSR1) != 0 || install_game_handler(SIGUSR2) != 0 ||
        install_game_handler(SIGINT) != 0 || install_game_handler(SIGTERM) != 0 ||
        install_game_handler((int)rt_signal) != 0) {
        perror("sigaction");
        return 1;
    }

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        pid_t parent = getppid();
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
            perror("prctl");
            _exit(1);
        }
        if (getppid() != parent) {
            _exit(0);
        }
        srandom((unsigned)time(NULL) ^ (unsigned)getpid());
        if (play_all_rounds(0, parent, upper, &suspend_mask) != 0) {
            fflush(stdout);
            _exit(1);
        }
        fflush(stdout);
        _exit(0);
    }

    child_pid = pid;
    memset(&stop_action, 0, sizeof stop_action);
    stop_action.sa_handler = handle_stop;
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGCHLD, &stop_action, NULL);

    srandom((unsigned)time(NULL) ^ (unsigned)getpid());
    if (play_all_rounds(1, pid, upper, &suspend_mask) != 0) {
        finish_parent();
        return 1;
    }

    finish_parent();
    printf("Игра завершена, раундов: %d\n", ROUNDS);
    fflush(stdout);
    return 0;
}
