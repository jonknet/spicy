// Copyright (c) 2020 by the Zeek Project. See LICENSE for details.

#pragma once
#include <sys/resource.h>

#include <memory>
#include <vector>

#include <hilti/rt/3rdparty/libaco/aco.h>
#include <hilti/rt/context.h>
#include <hilti/rt/debug-logger.h>
#include <hilti/rt/init.h>

// We collect all (or most) of the runtime's global state centrally. That's
// 1st good to see what we have (global state should be minimal) and 2nd
// helpful to ensure that JIT maps things correctly. Note that all code
// accessing any of this state is in charge of ensuring thread-safety itself.
// These globals are generally initialized through hilti::rt::init();
//
// TODO(robin): Accesses to global state are *not* completely thread-safe yet.

namespace hilti::rt {
struct Configuration;

namespace detail {
class DebugLogger;
}

} // namespace hilti::rt

namespace hilti::rt::detail {

/** Struct capturing all truely global runtime state. */
struct GlobalState {
    GlobalState();
    ~GlobalState();

    GlobalState(const GlobalState&) = delete;
    GlobalState(GlobalState&&) = delete;
    GlobalState& operator=(const GlobalState&) = delete;
    GlobalState& operator=(GlobalState&&) = delete;

    /** True once `hilit::init()`` has finished. */
    bool runtime_is_initialized = false;

    /** If not zero, `Configuration::abort_on_exception` is disabled. */
    int disable_abort_on_exceptions = 0;

    /** Resource usage at library initialization time. */
    ResourceUsage resource_usage_init;

    /** The runtime's configuration. */
    std::unique_ptr<hilti::rt::Configuration> configuration;

    /** Debug logger recording runtime diagnostics. */
    std::unique_ptr<hilti::rt::detail::DebugLogger> debug_logger;

    /** The context for the main thread. */
    std::unique_ptr<hilti::rt::Context> master_context;

    /** Cache of previously used fibers available for reuse. */
    std::vector<std::unique_ptr<Fiber>> fiber_cache;

    /**
     * List of HILTI modules registered with the runtime. This is filled through `registerModule()`, which in turn gets
     * called through a module's global constructors at initialization time.
     *
     * @note Must come last in this struct as destroying other fields may
     * still need this information.
     */
    std::vector<hilti::rt::detail::HiltiModule> hilti_modules;

    /** Shared stack for fiber execution. */
    std::unique_ptr<aco_share_stack_t, void (*)(aco_share_stack_t*)> share_st;

    /** Pointer to the coroutine controlling fiber execution. */
    std::unique_ptr<aco_t, void (*)(aco_t*)> main_co;
};

/**
 * Pointer to the global state singleton. Do not access directly, use
 * `globalState()` instead.
 */
extern GlobalState* __global_state;

/** Creates the global state singleton. */
extern GlobalState* createGlobalState();

/**
 * Returns the global state singleton. This creates the state the first time
 * it's called.
 */
inline auto globalState() {
    if ( __global_state )
        return __global_state;

    return createGlobalState();
}

/** Returns the current context's array of HILTI global variables. */
inline auto hiltiGlobals() {
    assert(context::detail::current());
    return context::detail::current()->hilti_globals;
}

/**
 * Returns the current context's set of a  HILTI module's global variables.
 *
 * @param idx module's index inside the array of HILTI global variables;
 * this is determined by the HILTI linker
 */
template<typename T>
inline auto moduleGlobals(unsigned int idx) {
    const auto& globals = hiltiGlobals();

    assert(idx < globals.size());

    return std::static_pointer_cast<T>(globals[idx]);
}

/**
 * Initialized the current context's set of a HILTI module's global
 * variables.
 *
 * @param idx module's index inside the array of HILTI global variables;
 * this is determined by the HILTI linker
 */
template<typename T>
inline auto initModuleGlobals(unsigned int idx) {
    if ( context::detail::current()->hilti_globals.size() <= idx )
        context::detail::current()->hilti_globals.resize(idx + 1);

    context::detail::current()->hilti_globals[idx] = std::make_shared<T>();
}

} // namespace hilti::rt::detail
