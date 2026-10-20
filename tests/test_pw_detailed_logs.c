/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Settings > Logging's folders (src/pw_detailed_logs.h): the setting, address masking, the folders' pruning and
 * pending hand-over, and the copies' bounds. */
#include "../src/pw_detailed_logs.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char base[64];

static void put(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && !fclose(file));
}

static long size_of(const char *path)
{
    struct stat status;
    return stat(path, &status) ? -1 : (long)status.st_size;
}

static char *slurp(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    assert(text && fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = 0;
    fclose(file);
    if (length) *length = (size_t)size;
    return text;
}

static unsigned folders(const char *root)
{
    unsigned count = 0;
    DIR *directory = opendir(root);
    assert(directory);
    for (struct dirent *entry; (entry = readdir(directory));) count += entry->d_name[0] >= '0' && entry->d_name[0] <= '9';
    closedir(directory);
    return count;
}

/* rm -r of what the test made: two levels of folders and files */
static void clean(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory) {
        unlink(path);
        return;
    }
    char child[512];
    for (struct dirent *entry; (entry = readdir(directory));) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        clean(child);
    }
    closedir(directory);
    rmdir(path);
}

static void setting(void)
{
    char path[128];
    int enabled = 1;
    snprintf(path, sizeof(path), "%s/logging.txt", base);
    assert(pw_detailed_logs_load(&enabled, path) == ENOENT && enabled == 0);
    assert(!pw_detailed_logs_save(1, path));
    assert(!pw_detailed_logs_load(&enabled, path) && enabled == 1);
    assert(!pw_detailed_logs_save(0, path));
    assert(!pw_detailed_logs_load(&enabled, path) && enabled == 0);
    put(path, "logging maybe\n");
    assert(pw_detailed_logs_load(&enabled, path) == EINVAL && enabled == 0);
    put(path, "game-mode on\n");
    assert(pw_detailed_logs_load(&enabled, path) == EINVAL && enabled == 0);
    unlink(path);
}

/* The longest run of digits and dots in text, counted from its first digit. */
static size_t longest_run(const char *text, size_t n)
{
    size_t longest = 0;
    for (size_t i = 0; i < n;) {
        if (!(text[i] == '.' || (text[i] >= '0' && text[i] <= '9'))) { ++i; continue; }
        size_t first = n, j = i;
        for (; j < n && (text[j] == '.' || (text[j] >= '0' && text[j] <= '9')); ++j)
            if (first == n && text[j] != '.') first = j;
        if (first < j && j - first > longest) longest = j - first;
        i = j;
    }
    return longest;
}

/* Random text masked whole and in random chunks as pw_detailed_logs_stream feeds them: the chunks never mask less,
 * and exactly as much when no run is longer than the carry. */
static void random_chunks(void)
{
    static const char alphabet[] = "0123456789....  :a\n";
    char whole[256], chunked[256], work[256 + PW_DETAILED_LOGS_CARRY];
    unsigned exact = 0;
    srand(7);
    for (int round = 0; round < 20000; ++round) {
        const size_t n = 1 + (size_t)rand() % 200;
        for (size_t i = 0; i < n; ++i) whole[i] = alphabet[(size_t)rand() % (sizeof(alphabet) - 1)];
        memcpy(chunked, whole, n);
        const size_t longest = longest_run(whole, n);
        assert(!pw_detailed_logs_mask(whole, n, 1, NULL));
        size_t done = 0, carry = 0, out = 0;
        int continued = 0;
        for (;;) {
            size_t take = 1 + (size_t)rand() % 23;
            if (take > n - done) take = n - done;
            memcpy(work + carry, chunked + done, take);
            done += take;
            const size_t have = carry + take;
            carry = pw_detailed_logs_mask(work, have, done == n, &continued);
            assert(carry <= PW_DETAILED_LOGS_CARRY);
            memcpy(chunked + out, work, have - carry);
            out += have - carry;
            memmove(work, work + have - carry, carry);
            if (done == n) break;
        }
        assert(out == n && !carry);
        for (size_t i = 0; i < n; ++i) assert(whole[i] != 'x' || chunked[i] == 'x');
        if (longest <= PW_DETAILED_LOGS_CARRY) {
            assert(!memcmp(whole, chunked, n));
            ++exact;
        }
    }
    assert(exact > 10000);
}

static void masking(void)
{
    char text[] = "ftp on 192.0.2.20:2121. wine 1234.567: v1.2.3.4.5 build 10.0.19041 at 203.0.113.7.";
    assert(!pw_detailed_logs_mask(text, strlen(text), 1, NULL));
    assert(!strcmp(text, "ftp on xxx.x.x.xx:2121. wine 1234.567: v1.2.3.4.5 build 10.0.19041 at xxx.x.xxx.x."));
    /* an address cut by a chunk's end waits for the next chunk */
    char chunk[64] = "peer 198.51";
    int continued = 0;
    const size_t held = pw_detailed_logs_mask(chunk, strlen(chunk), 0, &continued);
    assert(held == 6 && !continued && !strcmp(chunk, "peer 198.51"));
    char next[64] = "198.51.100.4 up";
    assert(!pw_detailed_logs_mask(next, strlen(next), 1, &continued) && !strcmp(next, "xxx.xx.xxx.x up"));
    /* a run longer than the carry is settled, and its rest in the next chunk is no new address */
    char run[64] = "0000000000000000000000000000000000.1";
    assert(!pw_detailed_logs_mask(run, strlen(run), 0, &continued) && continued);
    char rest[64] = "2.3.4 x 203.0.113.1";
    assert(!pw_detailed_logs_mask(rest, strlen(rest), 1, &continued) && !continued && !strcmp(rest, "2.3.4 x xxx.x.xxx.x"));
    random_chunks();

    /* stream: masked across reads, cut at the cap */
    int pipe_fds[2];
    assert(!pipe(pipe_fds));
    const char *input = "a 203.0.113.9\nb 8.8.8.8\n";
    assert(write(pipe_fds[1], input, strlen(input)) == (ssize_t)strlen(input));
    close(pipe_fds[1]);
    char path[128];
    snprintf(path, sizeof(path), "%s/stream.txt", base);
    int to = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    unsigned long long written = 0;
    assert(!pw_detailed_logs_stream(pipe_fds[0], to, 15, &written) && written == 15);
    close(to);
    close(pipe_fds[0]);
    char *got = slurp(path, NULL);
    assert(!strcmp(got, "a xxx.x.xxx.x\nb"));
    free(got);
    unlink(path);
}

static void folders_and_pending(void)
{
    char root[128], folder[PW_DETAILED_LOGS_PATH], again[PW_DETAILED_LOGS_PATH], path[512];
    snprintf(root, sizeof(root), "%s/detailed", base);
    unsigned removed = 99;
    assert(pw_detailed_logs_pending(root, folder, sizeof(folder)) == ENOENT);
    /* 2026-10-20 14:03:27 UTC */
    const time_t at = 1792505007;
    assert(!pw_detailed_logs_begin(root, 3, at, folder, sizeof(folder), &removed) && removed == 0);
    snprintf(path, sizeof(path), "%s/2026-10-20_14-03-27", root);
    assert(!strcmp(folder, path));
    snprintf(path, sizeof(path), "%s/README.txt", folder);
    char *readme = slurp(path, NULL);
    assert(strstr(readme, "Zip this whole folder") && strstr(readme, "IP address") && strstr(readme, "C:\\Games") &&
           strstr(readme, "Without klogsrv there is no klog.txt") && strstr(readme, "192.168.x.y"));
    /* the README holds no address itself: a player's folder passes the same scan as every log in it */
    char *masked = strdup(readme);
    assert(!pw_detailed_logs_mask(masked, strlen(masked), 1, NULL) && !strcmp(masked, readme));
    free(masked);
    free(readme);
    /* the same second again */
    assert(!pw_detailed_logs_begin(root, 3, at, again, sizeof(again), NULL));
    snprintf(path, sizeof(path), "%s/2026-10-20_14-03-27-2", root);
    assert(!strcmp(again, path));
    assert(!pw_detailed_logs_pending(root, folder, sizeof(folder)) && !strcmp(folder, again));
    assert(pw_detailed_logs_pending(root, folder, sizeof(folder)) == ENOENT);
    /* keep 3: the fourth folder's start removes the oldest, files and all */
    snprintf(path, sizeof(path), "%s/2026-10-20_14-03-27/title-early.log", root);
    put(path, "early\n");
    assert(!pw_detailed_logs_begin(root, 3, at + 60, folder, sizeof(folder), NULL) && folders(root) == 3);
    assert(!pw_detailed_logs_begin(root, 3, at + 120, folder, sizeof(folder), &removed) && folders(root) == 3);
    assert(removed == 2);   /* README.txt and title-early.log */
    snprintf(path, sizeof(path), "%s/2026-10-20_14-03-27", root);
    assert(size_of(path) < 0);
    /* a folder holding a folder is never emptied: it stays, and the next oldest goes */
    snprintf(path, sizeof(path), "%s/2026-10-20_14-03-27-2/kept", root);
    assert(!mkdir(path, 0777));
    assert(!pw_detailed_logs_begin(root, 3, at + 180, folder, sizeof(folder), NULL) && folders(root) == 3);
    assert(size_of(path) >= 0);
    snprintf(path, sizeof(path), "%s/2026-10-20_14-04-27", root);
    assert(size_of(path) < 0);
    /* a pending file naming anything else is refused, and consumed */
    snprintf(path, sizeof(path), "%s/pending", root);
    put(path, "/etc");
    assert(pw_detailed_logs_pending(root, folder, sizeof(folder)) == EINVAL && size_of(path) < 0);
    snprintf(again, sizeof(again), "%s/../x", root);
    put(path, again);
    assert(pw_detailed_logs_pending(root, folder, sizeof(folder)) == EINVAL);
    assert(pw_detailed_logs_begin(root, 0, at, folder, sizeof(folder), NULL) == EINVAL);
    clean(root);
}

static void sparse(const char *path, long long bytes, const char *tail)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0 && !ftruncate(fd, bytes) && lseek(fd, 0, SEEK_END) == bytes);
    assert(write(fd, tail, strlen(tail)) == (ssize_t)strlen(tail) && !close(fd));
}

static void filling(void)
{
    char root[128], folder[PW_DETAILED_LOGS_PATH], logs[128], session[128], path[512];
    snprintf(root, sizeof(root), "%s/detailed", base);
    snprintf(logs, sizeof(logs), "%s/logs", base);
    snprintf(session, sizeof(session), "%s/logs/0", base);
    assert(!mkdir(logs, 0777) && !mkdir(session, 0777));
    assert(!pw_detailed_logs_begin(root, 5, 1792505007, folder, sizeof(folder), NULL));
    snprintf(path, sizeof(path), "%s/title-last.log", logs);
    put(path, "Wine: session at 192.0.2.20\n");
    snprintf(path, sizeof(path), "%s/wine-stdout.txt", logs);
    put(path, "hello\n");
    snprintf(path, sizeof(path), "%s/game-frame.ppm", logs);
    put(path, "P6\n");
    snprintf(path, sizeof(path), "%s/wine-child-77.log", session);
    put(path, "child\n");
    snprintf(path, sizeof(path), "%s/README.txt", logs);   /* a name the folder has: skipped */
    put(path, "not the folder's\n");
    snprintf(path, sizeof(path), "%s/setup.txt", base);
    put(path, "language en-US\n");
    char missing[128];
    snprintf(missing, sizeof(missing), "%s/lapy-service.log", base);
    const char *sources[] = { logs, session, path, missing };
    unsigned copied = 0, left = 9;
    assert(!pw_detailed_logs_fill(folder, sources, 4, &copied, &left) && copied == 4 && left == 0);
    snprintf(path, sizeof(path), "%s/title-last.log", folder);
    char *text = slurp(path, NULL);
    assert(!strcmp(text, "Wine: session at xxx.x.x.xx\n"));
    free(text);
    snprintf(path, sizeof(path), "%s/game-frame.ppm", folder);
    assert(size_of(path) < 0);
    snprintf(path, sizeof(path), "%s/README.txt", folder);
    text = slurp(path, NULL);
    assert(strstr(text, "MihawkOS detailed logs"));
    free(text);
    snprintf(path, sizeof(path), "%s/wine-child-77.log", folder);
    assert(size_of(path) == 6);

    /* bounds: a file past PW_DETAILED_LOGS_FILE_BYTES keeps its end, and the folder stops at its own bound */
    char big[128];
    snprintf(big, sizeof(big), "%s/big", base);
    assert(!mkdir(big, 0777));
    for (int i = 0; i < 4; ++i) {
        snprintf(path, sizeof(path), "%s/huge-%d.log", big, i);
        sparse(path, (long long)PW_DETAILED_LOGS_FILE_BYTES + 100, "\nlast line 203.0.113.3\n");
    }
    const char *bigs[] = { big };
    assert(!pw_detailed_logs_fill(folder, bigs, 1, &copied, &left));
    assert(copied == 3 && left == 1);
    unsigned long long total = 0;
    DIR *directory = opendir(folder);
    for (struct dirent *entry; (entry = readdir(directory));) {
        snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
        struct stat status;
        if (!lstat(path, &status) && S_ISREG(status.st_mode)) {
            total += (unsigned long long)status.st_size;
            assert((unsigned long long)status.st_size <= PW_DETAILED_LOGS_FILE_BYTES);
        }
    }
    closedir(directory);
    assert(total <= PW_DETAILED_LOGS_FOLDER_BYTES);
    /* the folder's listing order decides which one was left out */
    for (int i = 0; i < 4; ++i) {
        snprintf(path, sizeof(path), "%s/huge-%d.log", folder, i);
        if (size_of(path) >= 0) break;
    }
    size_t length = 0;
    text = slurp(path, &length);
    assert(!strncmp(text, "[the first ", 11) && strstr(text, "left out"));
    assert(length > 23 && !strcmp(text + length - 23, "\nlast line xxx.x.xxx.x\n"));
    free(text);
    clean(big);
    clean(logs);
    clean(root);
    snprintf(path, sizeof(path), "%s/setup.txt", base);
    unlink(path);
}

int main(void)
{
    snprintf(base, sizeof(base), "/tmp/proton-detailed-logs-XXXXXX");
    assert(mkdtemp(base));
    setting();
    masking();
    folders_and_pending();
    filling();
    assert(!rmdir(base));
    puts("detailed logs: setting, masking, pruning, pending, bounded copies");
    return 0;
}
