#pragma once

#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>

namespace fas::detail
{
  //! @brief Type erased base for all future states
  class FutureStateBase
  {
  public:
    virtual ~FutureStateBase() = default;
  };

  //! @brief Base class and interface for continuations
  class FutureContinuation
  {
  public:
    virtual ~FutureContinuation() = default;
    virtual std::pair<std::unique_ptr<FutureContinuation>, std::shared_ptr<FutureStateBase>>
    continueWith(FutureStateBase& completedFuture) = 0;
  };

  //! @brief Shared state of a task
  template <class T> class FutureState : public FutureStateBase
  {
  public:
    bool isReady() const
    {
      const std::lock_guard<std::mutex> lock(_mutex);
      return readyImpl();
    }
    std::unique_ptr<FutureContinuation> next()
    {
      assert(readyImpl());
      return std::move(_cont);
    }
    std::unique_ptr<FutureContinuation> chain(std::unique_ptr<FutureContinuation> cont)
    {
      const std::lock_guard<std::mutex> lock(_mutex);
      assert(!_cont);
      if (readyImpl())
        return cont;

      _cont = std::move(cont);
      return nullptr;
    }
    void setValue(T value)
    {
      {
        const std::lock_guard<std::mutex> lock(_mutex);
        assert(!readyImpl());
        _value = std::move(value);
      }
      _readyCondition.notify_all();
    }
    void waitReady()
    {
      std::unique_lock<std::mutex> lock(_mutex);
      while (!readyImpl())
        _readyCondition.wait(lock);
    }
    T extractValue()
    {
      // We don't need to synchronize this, as we know that once the state is ready, it cannot be set again
      return std::move(*_value);
    }

  protected:
    virtual bool readyImpl() const { return _value.has_value(); }

  private:
    std::condition_variable _readyCondition;
    mutable std::mutex _mutex;
    std::optional<T> _value;
    std::unique_ptr<FutureContinuation> _cont;
  };

  void executeFuture(std::pair<std::unique_ptr<FutureContinuation>, std::shared_ptr<FutureStateBase>> next)
  {
    while (next.first)
      next = next.first->continueWith(*next.second);
  }

  template <class T, class Executor, class Fn> class FutureContinuationThen : public FutureContinuation
  {
  public:
    using U = std::decay_t<std::invoke_result_t<Fn, T>>;

    FutureContinuationThen(Executor executor, Fn continuation, std::shared_ptr<FutureState<U>> sstateNext)
        : _executor(std::move(executor)), _continuationFn(std::move(continuation)), _sstateNext(sstateNext)
    {
    }

    std::pair<std::unique_ptr<FutureContinuation>, std::shared_ptr<FutureStateBase>>
    continueWith(FutureStateBase& completedFuture) override
    {
      T val = static_cast<FutureState<T>&>(completedFuture).extractValue();

      _executor.spawn(
          [sstateNext = std::move(_sstateNext), continuationFn = std::move(_continuationFn), val = std::move(val)]() {
            sstateNext->setValue(continuationFn(std::move(val)));

            auto continuation = sstateNext->next();
            detail::executeFuture({std::move(continuation), std::move(sstateNext)});
          });
      return {nullptr, nullptr};
    }

  private:
    Executor _executor;
    Fn _continuationFn;
    std::shared_ptr<FutureState<U>> _sstateNext;
  };

} // namespace fas::detail

namespace fas
{
  /**
   * @brief Class representing a value which will be available in the future
   *
   * The class is similar to std::future, but additionally allows cancellation
   */
  template <class T> class Future
  {
  public:
    Future() : _sstate(nullptr) {}
    Future(std::shared_ptr<detail::FutureState<T>> sstate) : _sstate(std::move(sstate)) {}
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) = default;
    Future& operator=(Future&&) = default;

    void reset()
    {
      // TODO: Cancellation
      _sstate = nullptr;
    }

    bool isValid() const { return _sstate != nullptr; }
    bool isReady() const { return _sstate && _sstate->isReady(); }

    const T get()
    {
      assert(isValid());
      _sstate->waitReady();
      return _sstate->extractValue();
    }

    template <class Executor, class Fn>
    Future<std::decay_t<std::invoke_result_t<std::decay_t<Fn>, T>>> then(Executor executor, Fn&& continuation) &&
    {
      assert(isValid());

      using U = std::decay_t<std::invoke_result_t<std::decay_t<Fn>, T>>;
      using ContinuationT = detail::FutureContinuationThen<T, Executor, std::decay_t<Fn>>;

      Future<U> future;
      future._sstate = std::make_shared<detail::FutureState<U>>();
      auto next = _sstate->chain(
          std::make_unique<ContinuationT>(std::move(executor), std::forward<Fn>(continuation), future._sstate));

      detail::executeFuture({std::move(next), std::move(_sstate)});
      return future;
    }

  private:
    template <class U> friend class Future;

    std::shared_ptr<detail::FutureState<T>> _sstate;
  };

  /**
   * @brief Producing end of a Future/Promise pair
   *
   * Instances of this class can be used to resolve a future, by calling the `resolve()` function.
   *
   * @see fass::makePromise
   */
  template <class T> class Promise
  {
  public:
    Promise(std::shared_ptr<detail::FutureState<T>> sstate) : _sstate(sstate) {}

    void resolve(T value)
    {
      _sstate->setValue(std::move(value));
      detail::executeFuture({_sstate->next(), _sstate});
    }

  private:
    std::shared_ptr<detail::FutureState<T>> _sstate;
  };

  /**
   * @brief Creates a Future/Promise pair
   */
  template <class T> std::pair<Future<T>, Promise<T>> makePromise()
  {
    auto sstate = std::make_shared<detail::FutureState<T>>();
    Future<T> future(sstate);
    Promise<T> promise(std::move(sstate));
    return {std::move(future), std::move(promise)};
  }
} // namespace fas
