#pragma once
#include <Windows.h>

namespace fp::Mod
{
    void Initialize(HMODULE module);
    void Shutdown(bool processExiting);
}
