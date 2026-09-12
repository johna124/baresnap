#include "brs_remote.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: brs-remote-test /path/to/baresnap-remote\n");
        return 1;
    }

    const char *agent_path = argv[1];

    int pipe_to_agent[2];
    int pipe_from_agent[2];
    if (pipe(pipe_to_agent) != 0) { perror("pipe"); return 1; }
    if (pipe(pipe_from_agent) != 0) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipe_to_agent[1]);
        close(pipe_from_agent[0]);
        dup2(pipe_to_agent[0], STDIN_FILENO);
        dup2(pipe_from_agent[1], STDOUT_FILENO);
        close(pipe_to_agent[0]);
        close(pipe_from_agent[1]);

        char repo_path[] = "/tmp/brs_test_XXXXXX";
        if (!mkdtemp(repo_path)) { perror("mkdtemp"); return 1; }

        execl(agent_path, agent_path, "--repo", repo_path, NULL);
        perror("execl");
        return 1;
    }

    close(pipe_to_agent[0]);
    close(pipe_from_agent[1]);

    BrsRemote *r = brs_remote_connect(pipe_from_agent[0], pipe_to_agent[1]);
    if (!r) {
        fprintf(stderr, "Cannot connect to agent\n");
        return 1;
    }

    int failures = 0;

    /* Test 1: mkdir */
    if (brs_remote_mkdir(r, "testdir", 0755) != 0) {
        printf("FAIL: mkdir\n"); failures++;
    } else {
        printf("PASS: mkdir\n");
    }

    /* Test 2: open + write */
    const char *data = "Hello from remote!";
    int handle = brs_remote_open(r, "testdir/test.txt", BRS_OPEN_WRITE | BRS_OPEN_CREAT);
    if (handle < 0) {
        printf("FAIL: open for write\n"); failures++;
    } else {
        int written = brs_remote_write(r, handle, 0, data, strlen(data));
        if (written != (int)strlen(data)) {
            printf("FAIL: write\n"); failures++;
        } else {
            printf("PASS: write\n");
        }
        brs_remote_close(r, handle);
    }

    /* Test 3: open + read */
    handle = brs_remote_open(r, "testdir/test.txt", BRS_OPEN_READ);
    if (handle < 0) {
        printf("FAIL: open for read\n"); failures++;
    } else {
        char buf[256];
        int n = brs_remote_read(r, handle, 0, buf, sizeof(buf));
        if (n != (int)strlen(data) || memcmp(buf, data, n) != 0) {
            printf("FAIL: read\n"); failures++;
        } else {
            printf("PASS: read\n");
        }
        brs_remote_close(r, handle);
    }

    /* Test 4: list */
    char **names = NULL;
    size_t count = 0;
    if (brs_remote_list(r, "testdir", &names, &count) != 0) {
        printf("FAIL: list\n"); failures++;
    } else if (count != 1 || strcmp(names[0], "test.txt") != 0) {
        printf("FAIL: list content\n"); failures++;
    } else {
        printf("PASS: list\n");
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);

    /* Test 5: stat */
    uint64_t size;
    uint32_t mode;
    if (brs_remote_stat(r, "testdir/test.txt", &size, &mode) != 0) {
        printf("FAIL: stat\n"); failures++;
    } else if (size != strlen(data)) {
        printf("FAIL: stat size\n"); failures++;
    } else {
        printf("PASS: stat\n");
    }

    /* Test 6: rename */
    if (brs_remote_rename(r, "testdir/test.txt", "testdir/renamed.txt") != 0) {
        printf("FAIL: rename\n"); failures++;
    } else {
        printf("PASS: rename\n");
    }

    /* Test 7: unlink */
    if (brs_remote_unlink(r, "testdir/renamed.txt") != 0) {
        printf("FAIL: unlink\n"); failures++;
    } else {
        printf("PASS: unlink\n");
    }

    brs_remote_disconnect(r);
    waitpid(pid, NULL, 0);

    if (failures == 0) {
        printf("\nAll tests passed!\n");
        return 0;
    } else {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }
}
