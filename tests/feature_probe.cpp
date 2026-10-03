#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <expected>
#include <filesystem>
#include <span>
#include <stop_token>
#include <thread>

static_assert(__cplusplus >= 202400L);
static_assert(__cpp_lib_expected >= 202202L);
static_assert(__cpp_lib_jthread >= 201911L);
static_assert(__cpp_lib_span >= 202002L);
static_assert(__cpp_lib_filesystem >= 201703L);

int main()
{
    std::expected<int, int> value = 2;
    std::span<int> values(&*value, 1);

    std::jthread worker(
        [](std::stop_token stop)
        {
            static_cast<void>(stop.stop_requested());
        });

    const auto path = std::filesystem::current_path();
    ID2D1Factory* factory = nullptr;
    const auto result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);

    if (factory)
    {
        factory->Release();
    }

    return !path.empty() && values.front() == 2 && SUCCEEDED(result) ? 0 : 1;
}
