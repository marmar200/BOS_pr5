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
#define MSG_READY 1
#define MSG_GUESS 2
#define MSG_ANSWER 3

struct message {
    int type;
    int value;
};

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t child_pid = 0;

static void handle_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
    if (child_pid > 0) {
        kill((pid_t)child_pid, SIGTERM);
    }
}

static int write_message(int fd, int type, int value)
{
    struct message message;
    const char *bytes;
    size_t left;

    if (stop_requested) {
        return -1;
    }

    message.type = type;
    message.value = value;
    bytes = (const char *)&message;
    left = sizeof message;

    while (left > 0) {
        ssize_t count = write(fd, bytes, left);
        if (count < 0) {
            if (errno == EINTR) {
                if (stop_requested) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (count == 0) {
            return -1;
        }
        bytes += count;
        left -= (size_t)count;
    }

    return 0;
}

static int read_message(int fd, struct message *message)
{
    char *bytes = (char *)message;
    size_t got = 0;

    while (got < sizeof *message) {
        ssize_t count;

        if (stop_requested) {
            return -1;
        }
        count = read(fd, bytes + got, sizeof *message - got);
        if (count < 0) {
            if (errno == EINTR) {
                if (stop_requested) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
        if (count == 0) {
            return -1;
        }
        got += (size_t)count;
    }

    return 0;
}

static int think_round(int read_fd, int write_fd, int upper, int round, int thinker_id)
{
    struct message message;
    int secret = 1 + (int)(random() % upper);

    printf("Раунд %d. Загадывает игрок %d.\n", round, thinker_id);
    fflush(stdout);

    if (write_message(write_fd, MSG_READY, 0) != 0) {
        return -1;
    }

    for (;;) {
        if (read_message(read_fd, &message) != 0) {
            return -1;
        }
        if (message.type != MSG_GUESS || message.value < 1 || message.value > upper) {
            return -1;
        }
        if (message.value == secret) {
            return write_message(write_fd, MSG_ANSWER, 0);
        }
        if (message.value < secret) {
            if (write_message(write_fd, MSG_ANSWER, -1) != 0) {
                return -1;
            }
        } else if (write_message(write_fd, MSG_ANSWER, 1) != 0) {
            return -1;
        }
    }
}

static int guess_round(int read_fd, int write_fd, int upper, int guesser_id)
{
    struct message message;
    struct timespec start;
    struct timespec finish;
    int low = 1;
    int high = upper;
    int attempts = 0;

    if (read_message(read_fd, &message) != 0 || message.type != MSG_READY) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    while (low <= high) {
        int guess = low + (high - low) / 2;
        attempts++;
        if (write_message(write_fd, MSG_GUESS, guess) != 0 || read_message(read_fd, &message) != 0 ||
            message.type != MSG_ANSWER) {
            return -1;
        }

        if (message.value == 0) {
            clock_gettime(CLOCK_MONOTONIC, &finish);
            printf("Игрок %d: попытка %d (число %d) — угадал\n", guesser_id, attempts, guess);
            printf("Игрок %d угадал число %d за %d попыток, время %.3f с\n",
                   guesser_id, guess, attempts,
                   (finish.tv_sec - start.tv_sec) + (finish.tv_nsec - start.tv_nsec) / 1000000000.0);
            fflush(stdout);
            return 0;
        }

        if (message.value < 0) {
            printf("Игрок %d: попытка %d (число %d) — загаданное число больше\n", guesser_id, attempts, guess);
            low = guess + 1;
        } else {
            printf("Игрок %d: попытка %d (число %d) — загаданное число меньше\n", guesser_id, attempts, guess);
            high = guess - 1;
        }
        fflush(stdout);
    }

    fprintf(stderr, "pr5: ответы противоречат друг другу\n");
    return -1;
}

static int play_all_rounds(int is_parent, int read_fd, int write_fd, int upper)
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
            if (think_round(read_fd, write_fd, upper, round, my_id) != 0) {
                return -1;
            }
        } else if (guess_round(read_fd, write_fd, upper, my_id) != 0) {
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
        if (end == argv[1] || *end != '\0' || value < 1 || value > 1000000) {
            return -1;
        }
        return (int)value;
    }

    printf("Введите N (верхняя граница, от 1 до 1000000): ");
    fflush(stdout);
    {
        int value = 0;
        if (scanf("%d", &value) != 1 || value < 1 || value > 1000000) {
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

static void close_pair(int fds[2])
{
    if (fds[0] >= 0) {
        close(fds[0]);
        fds[0] = -1;
    }
    if (fds[1] >= 0) {
        close(fds[1]);
        fds[1] = -1;
    }
}

int main(int argc, char *argv[])
{
    int upper = read_upper(argc, argv);
    int to_child[2] = {-1, -1};
    int to_parent[2] = {-1, -1};
    struct sigaction stop_action;
    pid_t pid;

    if (upper < 0) {
        fprintf(stderr, "Использование: %s [N]\nN — целое от 1 до 1000000.\n", argv[0]);
        return 1;
    }

    memset(&stop_action, 0, sizeof stop_action);
    stop_action.sa_handler = handle_stop;
    sigemptyset(&stop_action.sa_mask);
    sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
    sigaction(SIGCHLD, &stop_action, NULL);
    stop_action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &stop_action, NULL);

    if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
        perror("pipe");
        close_pair(to_child);
        close_pair(to_parent);
        return 1;
    }

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        close_pair(to_child);
        close_pair(to_parent);
        return 1;
    }

    if (pid == 0) {
        pid_t parent = getppid();
        int read_fd = to_child[0];
        int write_fd = to_parent[1];

        close(to_child[1]);
        close(to_parent[0]);
        {
            struct sigaction default_action;
            memset(&default_action, 0, sizeof default_action);
            default_action.sa_handler = SIG_DFL;
            sigaction(SIGCHLD, &default_action, NULL);
        }
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
            perror("prctl");
            _exit(1);
        }
        if (getppid() != parent) {
            _exit(0);
        }
        srandom((unsigned)time(NULL) ^ (unsigned)getpid());
        if (play_all_rounds(0, read_fd, write_fd, upper) != 0) {
            close(read_fd);
            close(write_fd);
            fflush(stdout);
            _exit(1);
        }
        close(read_fd);
        close(write_fd);
        fflush(stdout);
        _exit(0);
    }

    child_pid = pid;
    close(to_child[0]);
    close(to_parent[1]);
    srandom((unsigned)time(NULL) ^ (unsigned)getpid());
    if (play_all_rounds(1, to_parent[0], to_child[1], upper) != 0) {
        close(to_parent[0]);
        close(to_child[1]);
        finish_parent();
        return 1;
    }

    close(to_parent[0]);
    close(to_child[1]);
    finish_parent();
    printf("Игра завершена, раундов: %d\n", ROUNDS);
    fflush(stdout);
    return 0;
}
