/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a process does to its own code and to another process's memory, as Chromium's sandbox, crash reporters,
 * launchers and hooking libraries do: executable read-write memory, a patch of its own code through
 * VirtualProtect and through WriteProcessMemory, and in a suspended child: VirtualAllocEx, WriteProcessMemory,
 * ReadProcessMemory, VirtualProtectEx and a patch of the child's code, then the child runs and checks them.
 * Each step reports its own verdict, so one run shows every step that fails. */
#include <windows.h>

/* The parent writes these in the child, where the child's image puts them at the same addresses. */
static volatile unsigned shared_marker;
static volatile ULONG_PTR shared_allocation;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}
static void say_hex(const char *label, ULONG_PTR value)
{
    char text[64];
    int i = 0, shift;
    for (const char *p = label; *p && i < 40; ) text[i++] = *p++;
    text[i++] = '0'; text[i++] = 'x';
    for (shift = 60; shift >= 0; shift -= 4) text[i++] = "0123456789abcdef"[(value >> shift) & 15];
    text[i++] = '\n'; text[i] = 0;
    say(text);
}

static unsigned failures;
static void step(const char *name, BOOL ok)
{
    say("PS5_PROTON_REMOTE_MEMORY_STEP "); say(name); say(ok ? " ok\n" : " FAILED\n");
    if (!ok) { failures++; say_hex("  last error ", GetLastError()); }
}

/* Patched below: returns 1 as built, 2 once patched. Called through a volatile pointer, so the compiler cannot
 * fold the call into its result. */
__declspec(noinline) int patched_function(void) { return 1; }
static int (*volatile call_patched)(void) = patched_function;

static void copy_bytes(volatile unsigned char *to, const volatile unsigned char *from, unsigned count)
{
    while (count--) *to++ = *from++;
}

static BOOL contains(const char *text, const char *word)
{
    for (; *text; ++text) {
        const char *a = text, *b = word;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return TRUE;
    }
    return FALSE;
}

/* The child: the parent wrote shared_marker, filled the allocation it made here, and patched patched_function. */
static void child(void)
{
    const unsigned char *block = (const unsigned char *)shared_allocation;
    DWORD code = 0;
    if (shared_marker == 0x5a5aa5a5) code |= 1;
    if (block && block[0] == 0x11 && block[4095] == 0x22) code |= 2;
    if (call_patched() == 2) code |= 4;
    ExitProcess(0x100 | code);
}

void mainCRTStartup(void)
{
    static const unsigned char return_two[6] = {0xb8, 2, 0, 0, 0, 0xc3};  /* mov eax,2; ret */
    char path[MAX_PATH * 2], command[MAX_PATH * 2 + 16];
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION info;
    unsigned char *rwx, *remote, check[4096];
    DWORD old, exit_code = 0;
    SIZE_T done = 0;
    BOOL ok;

    if (contains(GetCommandLineA(), "--remote-memory-child")) child();

    /* 1. Executable read-write memory, as a JIT makes it. */
    rwx = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    step("rwx-allocate", rwx != NULL);
    if (rwx) {
        rwx[0] = 0xb8; rwx[1] = 42; rwx[2] = rwx[3] = rwx[4] = 0; rwx[5] = 0xc3;
        FlushInstructionCache(GetCurrentProcess(), rwx, 6);
        step("rwx-execute", ((int (*)(void))rwx)() == 42);
        rwx[1] = 43;
        FlushInstructionCache(GetCurrentProcess(), rwx, 6);
        step("rwx-rewrite-execute", ((int (*)(void))rwx)() == 43);
    }

    /* 2. Patching its own code, as hooking libraries do: VirtualProtect to RWX, write, protect back. */
    ok = VirtualProtect((void *)patched_function, 6, PAGE_EXECUTE_READWRITE, &old);
    step("own-code-protect-rwx", ok);
    if (ok) {
        unsigned char saved[6];
        copy_bytes(saved, (unsigned char *)patched_function, 6);
        copy_bytes((unsigned char *)patched_function, return_two, 6);
        FlushInstructionCache(GetCurrentProcess(), (void *)patched_function, 6);
        step("own-code-patched-call", call_patched() == 2);
        copy_bytes((unsigned char *)patched_function, saved, 6);
        FlushInstructionCache(GetCurrentProcess(), (void *)patched_function, 6);
        step("own-code-protect-back", VirtualProtect((void *)patched_function, 6, old, &old));
        step("own-code-restored-call", call_patched() == 1);
    }

    /* 3. The same through WriteProcessMemory on itself (a launcher's hook does this). */
    {
        unsigned char saved[6];
        copy_bytes(saved, (unsigned char *)patched_function, 6);
        ok = WriteProcessMemory(GetCurrentProcess(), (void *)patched_function, return_two, 6, &done) && done == 6;
        step("own-code-writeprocessmemory", ok);
        if (ok) {
            step("own-code-writeprocessmemory-call", call_patched() == 2);
            step("own-code-writeprocessmemory-restore",
                 WriteProcessMemory(GetCurrentProcess(), (void *)patched_function, saved, 6, &done) && done == 6);
        }
    }

    /* 4. Another process's memory, as a sandbox broker sets its target up before the target runs. */
    GetModuleFileNameA(NULL, path, sizeof(path));
    lstrcpyA(command, "\"");
    lstrcatA(command, path);
    lstrcatA(command, "\" --remote-memory-child");
    ok = CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &startup, &info);
    step("child-create-suspended", ok);
    if (ok) {
        unsigned marker = 0x5a5aa5a5;
        ULONG_PTR address;

        remote = VirtualAllocEx(info.hProcess, NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        step("child-virtualallocex", remote != NULL);
        if (remote) {
            unsigned char fill[4096];
            unsigned i;
            for (i = 0; i < sizeof(fill); ++i) fill[i] = 0x33;
            fill[0] = 0x11; fill[4095] = 0x22;
            step("child-write-allocation", WriteProcessMemory(info.hProcess, remote, fill, sizeof(fill), &done) &&
                                           done == sizeof(fill));
            step("child-read-allocation", ReadProcessMemory(info.hProcess, remote, check, sizeof(check), &done) &&
                                          done == sizeof(check) && check[0] == 0x11 && check[100] == 0x33 &&
                                          check[4095] == 0x22);
        }
        address = (ULONG_PTR)remote;
        step("child-write-image-data", WriteProcessMemory(info.hProcess, (void *)&shared_marker, &marker,
                                                          sizeof(marker), &done) && done == sizeof(marker));
        step("child-write-image-pointer", WriteProcessMemory(info.hProcess, (void *)&shared_allocation, &address,
                                                             sizeof(address), &done) && done == sizeof(address));
        ok = VirtualProtectEx(info.hProcess, (void *)patched_function, 6, PAGE_EXECUTE_READWRITE, &old);
        step("child-code-protect-rwx", ok);
        step("child-code-write", WriteProcessMemory(info.hProcess, (void *)patched_function, return_two, 6, &done) &&
                                 done == 6);
        if (ok) step("child-code-protect-back", VirtualProtectEx(info.hProcess, (void *)patched_function, 6, old, &old));
        step("child-resume", ResumeThread(info.hThread) != (DWORD)-1);
        step("child-exit", WaitForSingleObject(info.hProcess, 30000) == WAIT_OBJECT_0 &&
                           GetExitCodeProcess(info.hProcess, &exit_code));
        step("child-saw-image-data", (exit_code & 0x101) == 0x101);
        step("child-saw-allocation", (exit_code & 0x102) == 0x102);
        step("child-saw-code-patch", (exit_code & 0x104) == 0x104);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }

    if (failures) {
        say_hex("PS5_PROTON_REMOTE_MEMORY_PROBE FAIL: steps failed ", failures);
        ExitProcess(1);
    }
    say("PS5_PROTON_REMOTE_MEMORY_PROBE PASS: executable read-write memory, own code patched two ways, and a suspended "
        "child's memory allocated, written, read, reprotected and patched before it ran\n");
    ExitProcess(0);
}
