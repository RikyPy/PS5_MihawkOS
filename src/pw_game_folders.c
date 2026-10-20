/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_game_folders.h"
#include "pw_pe_arch.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int program_name(const char *name)
{
    const size_t length = strlen(name);
    return length > 4 && !strcasecmp(name + length - 4, ".exe") && strncasecmp(name, "unins", 5) &&
           strncasecmp(name, "UnityCrashHandler", 17);
}

/* The folder's top-level programs: how many, and the first one's name. */
static void count_programs(const char *path, struct PwGameFolder *folder)
{
    DIR *directory = opendir(path);
    if (!directory) return;
    for (struct dirent *entry; (entry = readdir(directory));) {
        char file[2 * PW_GAME_FOLDER_NAME + 2];
        struct stat status;
        struct PwPeInfo pe;
        if (!program_name(entry->d_name) || strlen(entry->d_name) >= PW_GAME_FOLDER_NAME ||
            snprintf(file, sizeof(file), "%s/%s", path, entry->d_name) >= (int)sizeof(file) ||
            stat(file, &status) || !S_ISREG(status.st_mode))
            continue;
        const enum PwPeArch arch = pw_pe_arch_file(file, &pe);
        if (!pw_pe_arch_runs_on_x64(arch) && arch != PW_PE_X86) continue;
        if (!folder->programs++) strcpy(folder->program, entry->d_name);
    }
    closedir(directory);
}

int pw_game_folders_scan(const char *games, PwGameFolderFound found, void *context)
{
    DIR *directory = opendir(games);
    if (!directory) return errno;
    for (struct dirent *entry; (entry = readdir(directory));) {
        char path[2 * PW_GAME_FOLDER_NAME + 2];
        struct stat status;
        struct PwGameFolder folder = {0};
        if (entry->d_name[0] == '.' || strlen(entry->d_name) >= sizeof(folder.name) ||
            snprintf(path, sizeof(path), "%s/%s", games, entry->d_name) >= (int)sizeof(path) ||
            stat(path, &status) || !S_ISDIR(status.st_mode))
            continue;
        strcpy(folder.name, entry->d_name);
        count_programs(path, &folder);
        found(context, &folder);
    }
    closedir(directory);
    return 0;
}

int pw_game_folder_covers(const char *name, const char *windows_path)
{
    static const char games[] = "c:\\games\\";
    const size_t length = strlen(name);
    for (size_t i = 0; i < sizeof(games) - 1; i++) {
        const char c = windows_path[i] == '/' ? '\\' : windows_path[i];
        if ((c | (c >= 'A' && c <= 'Z' ? 0x20 : 0)) != games[i]) return 0;
    }
    windows_path += sizeof(games) - 1;
    return !strncasecmp(windows_path, name, length) && (windows_path[length] == '\\' || windows_path[length] == '/');
}
