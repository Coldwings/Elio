#pragma once

/// Umbrella header for all synchronization primitives.
/// For finer-grained includes, use the individual headers:
///   - sync/mutex.hpp
///   - sync/shared_mutex.hpp
///   - sync/semaphore.hpp
///   - sync/event.hpp
///   - sync/channel.hpp
///   - sync/spinlock.hpp
///   - sync/condition_variable.hpp

#include "mutex.hpp"
#include "shared_mutex.hpp"
#include "semaphore.hpp"
#include "event.hpp"
#include "channel.hpp"
#include "spinlock.hpp"
#include "condition_variable.hpp"
