#include "../ui/LauncherScreen.hxx"
#include "test_support.hxx"
int main() {
    using namespace sf4e::ui;
    CHECK(StartWithoutSurface(LauncherRoot::Home));
    CHECK(!StartWithoutSurface(LauncherRoot::Recovery)&&!StartWithoutSurface(LauncherRoot::Updates));
    CHECK(StartWithoutSurface(LauncherRoot::Updates,true)); // Automatic check is optional.
    CHECK(StartWithoutSurface(LauncherRoot::Home,true));
    return 0;
}
