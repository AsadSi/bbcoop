/* bbcoop: Bloodborne.exe of the package made on a Mac or Linux PC (packaging/windows/package-cross.sh),
 * where PyInstaller cannot freeze the launcher for Windows: it runs the launcher with the Python next to
 * it, `python\python.exe -s -E launcher\bbport_launcher_win.py`, without a console window, and waits
 * for it (Play Bloodborne.exe's --play goes through it, so Steam sees the game running). Extra
 * arguments are passed on. As play.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
    (void)instance, (void)previous, (void)show;
    static wchar_t dir[32768], python[32768], line[32768];
    DWORD length = GetModuleFileNameW(NULL, dir, 32768);
    if (length == 0 || length >= 32768)
        return 1;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    swprintf(python, 32768, L"%ls\\python\\python.exe", dir);
    swprintf(line, 32768, L"\"%ls\" -s -E \"%ls\\launcher\\bbport_launcher_win.py\"%ls%ls", python, dir,
             arguments && *arguments ? L" " : L"", arguments ? arguments : L"");

    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(python, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &startup, &process)) {
        MessageBoxW(NULL, L"python\\python.exe was not found next to Bloodborne.exe: unzip the whole "
                          L"bbcoop-windows folder again.", L"Bloodborne", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
