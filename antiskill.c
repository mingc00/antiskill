#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <pwd.h>
#include <libgen.h>
#include <errno.h>

#define EX_SUCCESS 0
#define EX_ERROR 1
#define EX_FATAL 2
#define EX_INTER 3
#define EX_TLE 4
#define EX_MLE 5
#define EX_RE 6

#define error(ret, msg) perror(msg);exit(ret);

char tmp_dir_template[] = "/tmp/sandbox.XXXXXX", *tmp_dir;
char *input_path = NULL, *output_path = NULL, *exec_path = NULL;
int mem_limit = 131072, time_limit = 1000;
int is_timeout = 0;
uid_t child_uid;
gid_t child_gid;
pid_t pid;


void parse_opt(int argc, char* const argv[]);
void init_env();
int sandbox();
void clear_env();
void usage();

void set_limit(int res, int limit);
int cp(char* src, char *dest);
int rm(char *target);
char* concat_path(char* dir, char* file);
int tv2ms(struct timeval tv);
void timeout();


int main(int argc, char *argv[])
{
    int ret;
    parse_opt(argc, argv);
    init_env();
    ret = sandbox();
    clear_env();
    return ret;
}

void parse_opt(int argc, char* const argv[])
{
    if(argc < 2) {
        usage(argv[0]);
        exit(EX_SUCCESS);
    }
    char c;
    while((c = getopt(argc, argv, "i:o:t:m:h")) != -1) {
        switch(c) {
            case 'i':
                input_path = optarg;
                break;
            case 'o':
                output_path = optarg;
                break;
            case 't':
                time_limit = atoi(optarg);
                break;
            case 'm':
                mem_limit = atoi(optarg);
                break;
            case 'h':
                usage(argv[0]);
                break;
        }
    }
    exec_path = argv[argc - 1];
}

void init_env() {
    struct passwd *nobody = getpwnam("nobody");
    int uid, gid;
    char* tmp_path;
    child_uid = nobody->pw_uid;
    child_gid = nobody->pw_gid;

    tmp_dir = mkdtemp(tmp_dir_template);
    chmod(tmp_dir, 00711);
    if(input_path != NULL) {
        cp(input_path, tmp_dir);
        tmp_path = concat_path(tmp_dir, basename(input_path));
        chmod(tmp_path, 00644);
        free(tmp_path);
    }
    cp(exec_path, tmp_dir);
    tmp_path = concat_path(tmp_dir, basename(exec_path));
    chmod(tmp_path, 00555);
    free(tmp_path);

    umask(0);
}

int sandbox() {
    int in, out, out_o;
    int status;
    struct rusage usage;

    pid = fork();
    if(pid == 0) {  // child
        if(chroot(tmp_dir)) {
            error(EX_INTER, "chroot fail");
        }
        chdir("/");
        setpgid(getpid(), 0);

        // redirct input file to STDIN
        if(input_path != NULL) {
            in = open(basename(input_path), O_RDONLY);
            dup2(in, STDIN_FILENO);
            close(in);
        }
        // redirct output file to STDIN
        if(output_path != NULL) {
            out = open(basename(output_path), O_WRONLY | O_CREAT | O_TRUNC, 00666);
            out_o = dup(STDOUT_FILENO);
            dup2(out, STDOUT_FILENO);
            close(out);
        }

        set_limit(RLIMIT_AS, (mem_limit + 10240) * 1024);
        set_limit(RLIMIT_CPU, time_limit / 1000 + 1);
        set_limit(RLIMIT_NOFILE, 10);
        set_limit(RLIMIT_NPROC, 20);

        setuid(child_uid);
        setgid(child_gid);
        char *bin_path = concat_path("", basename(exec_path));
        execl(bin_path, bin_path, NULL);
        dup2(out_o, STDOUT_FILENO);
        close(out_o);
        error(EX_INTER, "excute fail(forgot to link statically?)");
    } else {
        pid_t p = getpgrp();
        signal(SIGALRM, timeout);
        alarm((int) (time_limit / 1000) + 2);
        wait3(&status, WUNTRACED, &usage);
        if(WIFEXITED(status) && (WEXITSTATUS(status) == EX_INTER)) {
            printf("fatal\n");
            return EX_FATAL;
        }
        int time = tv2ms(usage.ru_utime) + tv2ms(usage.ru_stime);
        if((time > time_limit) || is_timeout) {
            printf("Time Limit Exceeded\n");
            return EX_TLE;
        }
        long memory = usage.ru_minflt * (getpagesize() >> 10);
        if(memory > mem_limit) {
            printf("Memory Limit Exceeded\n");
            return EX_MLE;
        }

        if(WIFEXITED(status)) {
            printf("%d %d %ld\n", WEXITSTATUS(status), time, memory);
        } else {
            printf("Program Killed\n");
        }

        if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
            return EX_RE;
        }

        return EX_SUCCESS;
    }
}

void clear_env() {
    if(output_path != NULL) {
        char *tmp_output_path = NULL, *filename;
        filename = basename(output_path);
        tmp_output_path = concat_path(tmp_dir, filename);
        cp(tmp_output_path, output_path);
        free(tmp_output_path);
    }
    rm(tmp_dir);
}

void usage(char* name) {
    printf("Usage: %s [OPTIONS] PROGRAM\n", name);
    printf("Options:\n"
            "  -t TIME_LIMIT in ms, positive int only (default: 1000)\n"
            "  -m MEMORY_LIMIT in kb, positive int only (default: 131072)\n"
            "  -i INPUT_PATH\n"
            "  -o OUTPUT_PATH\n"
            "  -h print this help\n\n"
            "Output:\n"
            "  1. exited: WEXITSTATUS TIME(ms) MEMORY(KB)\n"
            "  2. killed: message\n"
            "Notes: PROGRAM must be compiled statically!\n"
            );
    exit(EX_SUCCESS);
}

void set_limit(int res, int limit) {
    struct rlimit rlim;
    rlim.rlim_cur = rlim.rlim_max = limit;
    if(setrlimit(res, &rlim) != 0) {
        printf("limit %d fail", res);
        error(EX_INTER, "Set limit fail");
    }
}

void timeout() {
    if(pid > 0) {
        pid_t p = getpgid(pid);
        killpg(p, SIGKILL);
    }
    is_timeout = 1;
    alarm(0);
}

int cp(char* src, char *dest) {
    int ret;
    char* cmd = (char*) malloc(sizeof(char) * (strlen(src) + strlen(dest) + 17));
    sprintf(cmd, "cp %s %s 2>/dev/null", src, dest);
    ret = system(cmd);
    free(cmd);
    return ret;
}

int rm(char *target) {
    int ret;
    char* cmd = (char*) malloc(sizeof(char) * (strlen(target) + 8));
    sprintf(cmd, "rm -rf %s", target);
    ret = system(cmd);
    free(cmd);
    return ret;
}

char* concat_path(char* dir, char* file) {
    char* buffer = (char*) malloc(sizeof(char) * (strlen(dir) + strlen(file) + 2));
    sprintf(buffer, "%s/%s", dir, file);
    return buffer;
}

int tv2ms(struct timeval tv) {
    return (int) (tv.tv_usec / 1000) + tv.tv_sec * 1000;
}

