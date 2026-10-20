/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a launcher's update agent asks before it lets a game be played (docs/PLAN.md, the Unity game's "Update" review): the agent logs
 * "Failed to get volume path name" and "Build marked non-playable: Permissions check failure (2123)", and its strings name
 * GetNamedSecurityInfo, AccessCheck, SetNamedSecurityInfo and SetSecurityInfo. This asks the same Windows API questions of a folder
 * and prints every answer, so that the console's answers can be set beside stock Wine's on the host.
 *
 *   agent-permissions-probe [FOLDER]      FOLDER defaults to C:\Program Files (x86)\Example Game
 *
 * Prints "perm ..." lines and PS5_PROTON_PERMS DONE. It changes nothing but a scratch folder and a scratch registry key it removes. */
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void say(const char *format, ...)
{
    char text[1200];
    __builtin_va_list args;
    __builtin_va_start(args, format);
    wvsprintfA(text, format, args);
    __builtin_va_end(args);
    line(text);
}

static void sid_text(PSID sid, char *out, int size)
{
    LPSTR text = NULL;
    out[0] = 0;
    if (sid && ConvertSidToStringSidA(sid, &text)) {
        lstrcpynA(out, text, size);
        LocalFree(text);
    } else
        lstrcpynA(out, "(none)", size);
}

static void volume_path(const char *label, const char *path)
{
    char out[MAX_PATH];
    BOOL ok;
    SetLastError(0);
    ok = GetVolumePathNameA(path, out, sizeof(out));
    say("perm GetVolumePathName %s [%s] -> %d '%s' error=%lu\n", label, path, ok, ok ? out : "", GetLastError());
}

static void describe_acl(const char *what, const char *path)
{
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PSID owner = NULL, group = NULL;
    PACL dacl = NULL;
    char text[200];
    DWORD result = GetNamedSecurityInfoA(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        &owner, &group, &dacl, NULL, &descriptor);
    say("perm GetNamedSecurityInfo %s [%s] -> %lu dacl=%p\n", what, path, result, (void *)dacl);
    if (result) return;
    sid_text(owner, text, sizeof(text));
    say("perm   owner %s\n", text);
    sid_text(group, text, sizeof(text));
    say("perm   group %s\n", text);
    if (dacl) {
        ACL_SIZE_INFORMATION size = {0};
        if (GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation)) {
            DWORD i;
            say("perm   dacl has %lu entries\n", size.AceCount);
            for (i = 0; i < size.AceCount && i < 12; i++) {
                void *ace;
                if (GetAce(dacl, i, &ace)) {
                    ACCESS_ALLOWED_ACE *allowed = ace;
                    sid_text((PSID)&allowed->SidStart, text, sizeof(text));
                    say("perm   ace %lu type=%u flags=%#x mask=%#lx sid=%s\n", i, allowed->Header.AceType, allowed->Header.AceFlags, allowed->Mask, text);
                }
            }
        }
    } else
        say("perm   no dacl: everyone has full access\n");
    LocalFree(descriptor);
}

/* AccessCheck for the running user, as the Agent does for the folder it is about to install into. */
static void access_check(const char *path, DWORD generic)
{
    PSECURITY_DESCRIPTOR descriptor = NULL;
    HANDLE token = NULL, impersonation = NULL;
    GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    PRIVILEGE_SET privileges;
    DWORD privileges_size = sizeof(privileges), granted = 0, wanted = generic;
    BOOL status = FALSE, ok;
    DWORD result = GetNamedSecurityInfoA(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                        NULL, NULL, NULL, NULL, &descriptor);
    if (result) { say("perm AccessCheck [%s] no descriptor (%lu)\n", path, result); return; }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token) ||
        !DuplicateToken(token, SecurityImpersonation, &impersonation)) {
        say("perm AccessCheck [%s] no token (error %lu)\n", path, GetLastError());
        LocalFree(descriptor);
        return;
    }
    MapGenericMask(&wanted, &mapping);
    SetLastError(0);
    ok = AccessCheck(descriptor, impersonation, wanted, &mapping, &privileges, &privileges_size, &granted, &status);
    say("perm AccessCheck [%s] wanted=%#lx -> call %d granted=%#lx status=%d error=%lu\n", path, wanted, ok, granted, status, GetLastError());
    CloseHandle(impersonation);
    CloseHandle(token);
    LocalFree(descriptor);
}

static void who_am_i(void)
{
    HANDLE token = NULL;
    BYTE buffer[512];
    DWORD size = 0;
    char text[200];
    BOOL member = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = { { 0, 0, 0, 0, 0, 5 } };
    PSID users = NULL, administrators = NULL;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        sid_text(((TOKEN_USER *)buffer)->User.Sid, text, sizeof(text));
        say("perm token user %s\n", text);
    }
    if (token && GetTokenInformation(token, TokenGroups, buffer, sizeof(buffer), &size)) {
        TOKEN_GROUPS *groups = (TOKEN_GROUPS *)buffer;
        DWORD i;
        say("perm token has %lu groups\n", groups->GroupCount);
        for (i = 0; i < groups->GroupCount && i < 16; i++) {
            sid_text(groups->Groups[i].Sid, text, sizeof(text));
            say("perm   group %s attributes=%#lx\n", text, groups->Groups[i].Attributes);
        }
    }
    if (AllocateAndInitializeSid(&nt, 2, 32, 545, 0, 0, 0, 0, 0, 0, &users)) {
        member = FALSE;
        CheckTokenMembership(NULL, users, &member);
        say("perm member of BUILTIN\\Users: %d\n", member);
        FreeSid(users);
    }
    if (AllocateAndInitializeSid(&nt, 2, 32, 544, 0, 0, 0, 0, 0, 0, &administrators)) {
        member = FALSE;
        CheckTokenMembership(NULL, administrators, &member);
        say("perm member of BUILTIN\\Administrators: %d\n", member);
        FreeSid(administrators);
    }
    if (token) CloseHandle(token);
}

static void volume_info(const char *root)
{
    char name[MAX_PATH], fs[MAX_PATH];
    DWORD serial = 0, length = 0, flags = 0;
    BOOL ok = GetVolumeInformationA(root, name, sizeof(name), &serial, &length, &flags, fs, sizeof(fs));
    say("perm GetVolumeInformation [%s] -> %d fs='%s' flags=%#lx (persistent ACLs %s) serial=%#lx error=%lu\n", root, ok, ok ? fs : "", flags,
        (flags & FILE_PERSISTENT_ACLS) ? "yes" : "no", serial, GetLastError());
}

static void writes(const char *folder)
{
    char scratch[MAX_PATH];
    HANDLE file;
    PSID everyone = NULL;
    SID_IDENTIFIER_AUTHORITY world = { { 0, 0, 0, 0, 0, 1 } };
    EXPLICIT_ACCESSA access;
    PACL acl = NULL;
    DWORD result;
    HKEY key;
    LONG status;

    wsprintfA(scratch, "%s\\perm-probe-scratch", folder);
    CreateDirectoryA(scratch, NULL);
    describe_acl("scratch folder", scratch);
    access_check(scratch, GENERIC_READ | GENERIC_WRITE);
    wsprintfA(scratch, "%s\\perm-probe-scratch\\file.txt", folder);
    file = CreateFileA(scratch, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    say("perm create scratch file [%s] -> %d error=%lu\n", scratch, file != INVALID_HANDLE_VALUE, GetLastError());
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    describe_acl("scratch file", scratch);
    if (AllocateAndInitializeSid(&world, 1, 0, 0, 0, 0, 0, 0, 0, 0, &everyone)) {
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.pMultipleTrustee = NULL;
        access.Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = (LPSTR)everyone;
        result = SetEntriesInAclA(1, &access, NULL, &acl);
        say("perm SetEntriesInAcl -> %lu\n", result);
        if (!result) {
            result = SetNamedSecurityInfoA(scratch, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL);
            say("perm SetNamedSecurityInfo [%s] -> %lu\n", scratch, result);
            LocalFree(acl);
            describe_acl("scratch file after SetNamedSecurityInfo (Everyone full)", scratch);
            /* what the Agent wants of a folder it manages: Everyone may read and run, no more (its SetEntriesInAcl: 0x1f01ff for the owner, 0x1200a9 for the rest) */
            access.grfAccessPermissions = 0x1200a9;
            access.grfAccessMode = SET_ACCESS;
            acl = NULL;
            result = SetEntriesInAclA(1, &access, NULL, &acl);
            if (!result) {
                result = SetNamedSecurityInfoA(scratch, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | 0x80000000, NULL, NULL, acl, NULL);
                say("perm SetNamedSecurityInfo [%s] Everyone read only -> %lu\n", scratch, result);
                LocalFree(acl);
                describe_acl("scratch file after SetNamedSecurityInfo (Everyone read only)", scratch);
            }
        }
        FreeSid(everyone);
    }
    DeleteFileA(scratch);
    wsprintfA(scratch, "%s\\perm-probe-scratch", folder);
    RemoveDirectoryA(scratch);

    /* the registry key whose SetSecurityInfo the Agent logs as failing */
    status = RegCreateKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PermProbeScratch", 0, NULL, 0,
                             KEY_ALL_ACCESS | WRITE_DAC | READ_CONTROL, NULL, &key, NULL);
    say("perm registry key create -> %ld\n", status);
    if (!status) {
        PSECURITY_DESCRIPTOR descriptor = NULL;
        DWORD size = 0;
        result = GetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL, &descriptor);
        say("perm registry GetSecurityInfo -> %lu\n", result);
        if (!result) LocalFree(descriptor);
        result = SetSecurityInfo(key, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
        say("perm registry SetSecurityInfo (null dacl) -> %lu\n", result);
        (void)size;
        RegCloseKey(key);
        RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PermProbeScratch");
    }
}

/* GetVolumePathNameW asks NtQueryAttributesFile about the path and each parent, ending at the drive's root with its trailing backslash. */
static void attributes_of(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    DWORD plain;
    BOOL ok;
    SetLastError(0);
    plain = GetFileAttributesA(path);
    say("perm GetFileAttributes [%s] -> %#lx error=%lu\n", path, plain, plain == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0);
    SetLastError(0);
    ok = GetFileAttributesExA(path, GetFileExInfoStandard, &data);
    say("perm GetFileAttributesEx [%s] -> %d %#lx error=%lu\n", path, ok, ok ? data.dwFileAttributes : 0, ok ? 0 : GetLastError());
}

void mainCRTStartup(void)
{
    char folder[MAX_PATH], command[MAX_PATH * 2], file[MAX_PATH], parent[MAX_PATH];
    const char *c, *argument = NULL;
    char *slash;

    lstrcpyA(folder, "C:\\Program Files (x86)\\Example Game");
    lstrcpynA(command, GetCommandLineA(), sizeof(command));
    for (c = command; *c; ++c)
        if (*c == ' ' && c[1] && c[1] != '-') { argument = c + 1; break; }
    if (argument) {
        int length = lstrlenA(argument);
        lstrcpynA(folder, argument, sizeof(folder));
        if (length > 1 && folder[0] == '"') { lstrcpynA(folder, argument + 1, sizeof(folder)); folder[lstrlenA(folder) - 1] = 0; }
    }

    say("perm folder [%s] attributes=%#lx\n", folder, GetFileAttributesA(folder));
    who_am_i();
    volume_info("C:\\");
    attributes_of("C:\\windows");
    volume_path("backslashes", folder);
    {
        char forward[MAX_PATH];
        int i;
        lstrcpynA(forward, folder, sizeof(forward));
        for (i = 0; forward[i]; i++) if (forward[i] == '\\') forward[i] = '/';
        volume_path("forward slashes", forward);
        lstrcatA(forward, "/");
        volume_path("forward, trailing", forward);
        lstrcpynA(file, folder, sizeof(file));
        lstrcatA(file, "\\Example Game.exe");
        volume_path("a file inside", file);
        lstrcpynA(parent, folder, sizeof(parent));
        slash = parent + lstrlenA(parent);
        while (slash > parent && *slash != '\\') --slash;
        *slash = 0;
        volume_path("parent folder", parent);
        volume_path("a drive root", "C:\\");
        volume_path("a drive root, forward", "C:/");
    }
    attributes_of("C:\\");
    attributes_of("C:");
    attributes_of("C:\\Program Files (x86)");
    attributes_of("C:\\Program Files (x86)\\");
    attributes_of("\\\\?\\C:\\");
    attributes_of("\\\\?\\C:");
    attributes_of("\\\\.\\C:\\");
    describe_acl("the folder", folder);
    access_check(folder, GENERIC_READ | GENERIC_WRITE);
    access_check(folder, GENERIC_ALL);
    describe_acl("a file inside", file);
    describe_acl("the parent", parent);
    describe_acl("a drive root", "C:\\");
    writes(folder);
    line("PS5_PROTON_PERMS DONE\n");
    ExitProcess(0);
}
