#pragma once

#include <windows.h>

namespace Dimps {
    struct UserApp {
        typedef struct __staticMethods {
            void (*Steam_PostUpdate)();
        } __staticMethods;

        static void Locate(HMODULE peRoot);
        static __staticMethods staticMethods;
    };
}