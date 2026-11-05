#pragma once
// {7A1C3F52-9B4E-4D2A-8F61-2C5E9D10A347}
static const CLSID CLSID_DiaTweaksTap = {0x7a1c3f52, 0x9b4e, 0x4d2a, {0x8f, 0x61, 0x2c, 0x5e, 0x9d, 0x10, 0xa3, 0x47}};

#define DT_EVENT_RELOAD L"Local\\DiaTweaksWin_Reload"
#define DT_EVENT_DUMP   L"Local\\DiaTweaksWin_Dump"
#define DT_EVENT_DONE   L"Local\\DiaTweaksWin_Done"
// Followed by Dia's pid. ATTACHED is set once the add-on is attached to Dia's UI; RETRY asks an add-on
// that loaded but couldn't attach to try again.
#define DT_EVENT_ATTACHED L"Local\\DiaTweaksWin_Attached_"
#define DT_EVENT_RETRY    L"Local\\DiaTweaksWin_Retry_"
// The helper behind "Patch Dia however it's opened" (DiaLauncher.exe --watch): held while it runs, and
// set to make it stop.
#define DT_WATCH_MUTEX L"Local\\DiaTweaksWin_Watcher"
#define DT_WATCH_STOP  L"Local\\DiaTweaksWin_WatcherStop"
