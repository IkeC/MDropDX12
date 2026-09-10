#pragma once
//======================================================================
// Guarded<T> -- state that cannot be reached except under its own lock
//
// #11. `Engine` has 687 members across five threads, so "shared mutable
// state" is not a mistake anyone made; it is the default, because there is no
// boundary to cross and nothing signals when one is crossed. This is the
// boundary.
//
// The design is subtractive, and the omissions are the whole point:
//
//   * NO operator->, NO data(), NO get(). There is no way to obtain a
//     reference to the value except inside a callback that runs while the
//     lock is held -- which is exactly the mistake the tree has already
//     shipped twice: once with the render thread holding msg.c_str() into
//     m_errors across a swprintf (#9), and once with a ToolWindow thread
//     holding a PresetAnnotation* across another thread's erase.
//
//   * with() returns a DECAYED value. A callback that returns `T&` therefore
//     hands the caller a copy, not an alias. Without the decay, the one hole
//     left in the design would be `g.with([](T& v) -> T& { return v; })`,
//     which is short enough to write by accident.
//
//   * Non-copyable and non-movable. Copying a guarded object would copy the
//     value out from under whoever holds the lock.
//
// The mutex is a template parameter because a plain std::mutex does not fit
// every container. m_displayOutputs needed a recursive one: RefreshDisplaysTab
// is called both from a ToolWindow handler that already holds the lock and
// from a RenderCmd handler that does not (see m_displayOutputsMutex in
// engine.h). A with() callback cannot re-enter a plain mutex, so a fixed
// std::mutex would have ruled that container out entirely.
//
// Two rules the compiler cannot enforce, and which every callback must obey:
//
//   * Never nest two different guards without a written ordering rule.
//   * Never run a UI SendMessage, a modal dialog, or file I/O inside a
//     callback. SendMessage pumps, so it re-enters, and it will deadlock
//     against a ToolWindow thread parked in with() on the same guard.
//     Snapshot inside the lock and act outside it.
//======================================================================

#include <mutex>
#include <type_traits>
#include <utility>

namespace mdrop {

template <class T, class M = std::mutex>
class Guarded {
public:
  Guarded() = default;
  explicit Guarded(T value) : m_value(std::move(value)) {}

  Guarded(const Guarded&) = delete;
  Guarded& operator=(const Guarded&) = delete;
  Guarded(Guarded&&) = delete;
  Guarded& operator=(Guarded&&) = delete;

  // Run fn against the protected value with the lock held. The return type is
  // decayed, so a callback cannot hand back a reference that outlives it.
  template <class F>
  auto with(F&& fn) -> typename std::decay<decltype(fn(std::declval<T&>()))>::type {
    std::lock_guard<M> lock(m_mutex);
    return fn(m_value);
  }

  template <class F>
  auto with(F&& fn) const
      -> typename std::decay<decltype(fn(std::declval<const T&>()))>::type {
    std::lock_guard<M> lock(m_mutex);
    return fn(m_value);
  }

private:
  T m_value{};
  mutable M m_mutex;
};

}  // namespace mdrop
