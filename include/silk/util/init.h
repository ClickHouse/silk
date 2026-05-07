#pragma once

namespace silk
{

/**
 * Initialize all silk singletons in the right order. Call once at process startup before any silk calls
 */
void initialize() noexcept;

/**
 * Tear down silk singletons in reverse order.
 */
void destroy() noexcept;

} // namespace silk
