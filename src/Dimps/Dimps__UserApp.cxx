#include <windows.h>
#include "Dimps__UserApp.hxx"

using Dimps::UserApp;
UserApp::__staticMethods UserApp::staticMethods;

void UserApp::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    staticMethods.Steam_PostUpdate = (void(*)())(peRootOffset + 0x10e600);
}
