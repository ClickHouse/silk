#pragma once

#include <silk/util/platform.h>

#include <utility>

namespace silk
{

template <typename Function>
class ScopeGuard
{
public:
    explicit ScopeGuard(Function function) noexcept
        : function(std::move(function))
    {
    }
    ~ScopeGuard() noexcept { function(); }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard & operator=(const ScopeGuard &) = delete;
    ScopeGuard(ScopeGuard &&) = delete;
    ScopeGuard & operator=(ScopeGuard &&) = delete;

private:
    Function function;
};

struct ScopeGuardTag
{
};

template <typename Function>
ScopeGuard<Function> operator+(ScopeGuardTag, Function function) noexcept
{
    return ScopeGuard<Function>(std::move(function));
}

} // namespace silk

/** Run the trailing block when the enclosing scope exits. Use as: `SILK_SCOPE_EXIT { cleanup(p); };` */
#define SILK_SCOPE_EXIT const auto SILK_CONCAT(scopeGuard, __LINE__) = silk::ScopeGuardTag{} + [&]() noexcept
