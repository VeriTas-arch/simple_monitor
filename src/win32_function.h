#pragma once

#include <windows.h>

#include <cstring>
#include <type_traits>

namespace simple_monitor {

template <typename Function>
Function LoadOptionalFunction(HMODULE module, LPCSTR name) noexcept {
    static_assert(std::is_pointer_v<Function>);
    static_assert(std::is_function_v<std::remove_pointer_t<Function>>);

    const FARPROC address = module && name ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(Function) == sizeof(address));

    Function function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

}  // namespace simple_monitor
