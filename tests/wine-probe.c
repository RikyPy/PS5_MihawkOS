/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>

static const unsigned long long vector[4] = {
    0x1122334455667788ULL, 0x8877665544332211ULL, 0x123456789abcdef0ULL, 0xfedcba9876543210ULL
};
static unsigned saved_mxcsr;
static BOOL signal_ok;
static LONG CALLBACK fault_handler(EXCEPTION_POINTERS *exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->ExceptionInformation[1] != 8) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *context = exception->ContextRecord;
    signal_ok = context->MxCsr == saved_mxcsr &&
        context->FltSave.XmmRegisters[15].Low == vector[0] &&
        (unsigned long long)context->FltSave.XmmRegisters[15].High == vector[1];
    context->Rip += 3; /* movl 8(%rax), %eax */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI signal_event(void *event)
{
    SetEvent(event);
    return 0;
}

void mainCRTStartup(void)
{
    DWORD written;
    DWORD old_protection;
    unsigned long long restored[4];
    void *handler = AddVectoredExceptionHandler(1, fault_handler);
    __asm__ volatile("stmxcsr %0\n\tvmovdqu %2, %%ymm15\n\txor %%eax, %%eax\n\t"
                     "movl 8(%%rax), %%eax\n\tvmovdqu %%ymm15, %1"
                     : "=m"(saved_mxcsr), "=m"(restored) : "m"(vector)
                     : "rax", "xmm15", "memory");
    RemoveVectoredExceptionHandler(handler);
    for (unsigned i = 0; i < 4; ++i) signal_ok = signal_ok && restored[i] == vector[i];
    /* CLR reserves heaps near its DLL, outside Wine's initial PS5 regions. */
    unsigned char *code = VirtualAlloc((void *)0x6000000000ULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    BOOL memory_ok = code && VirtualAlloc(code, 0x1000, MEM_COMMIT, PAGE_READWRITE) == code;
    if (memory_ok) {
        code[0] = 0xb8; code[1] = 42; code[2] = 0; code[3] = 0; code[4] = 0; code[5] = 0xc3;
        memory_ok = VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &old_protection);
        if (memory_ok) memory_ok = ((int (*)(void))code)() == 42;
        if (memory_ok) memory_ok = VirtualProtect(code, 0x1000, PAGE_READWRITE, &old_protection);
        if (memory_ok) { code[1] = 43; memory_ok = code[0] == 0xb8 && code[1] == 43; }
    }
    if (code) VirtualFree(code, 0, MEM_RELEASE);
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE thread = event ? CreateThread(NULL, 0, signal_event, event, 0, NULL) : NULL;
    BOOL ok = signal_ok && memory_ok && thread && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    const char *message = ok ? "PS5_PROTON_PE64_PROBE PASS: signal FP/AVX, fixed memory, JIT, thread and event\r\n"
                             : "PS5_PROTON_PE64_PROBE FAIL: signal, memory, JIT, thread or event\r\n";
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, lstrlenA(message), &written, NULL);
    OutputDebugStringA(message);
    if (thread) { WaitForSingleObject(thread, 5000); CloseHandle(thread); }
    if (event) CloseHandle(event);
    ExitProcess(ok ? 0 : 1);
}
