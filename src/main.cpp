#include "wolfie_app.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    wolfie::WolfieApp app(instance);
    return app.run();
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR, int showCommand) {
    return wWinMain(instance, previousInstance, GetCommandLineW(), showCommand);
}
