#pragma once
#include <cassert>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <sstream>

#include <cps/future/error_code.h>
#include <cps/future/is_string.h>

#ifdef UNCAUGHT_EXCEPTION_DEBUGGING
#include <iostream>
#endif

namespace cps {

/**
 */
template<typename T>
class future {

public:
	/* Probably not very useful since the API is returning shared_ptr all over the shop */
	static std::unique_ptr<future<T>> create(
		const std::string &label = u8"unlabelled future"
	) {
		return std::unique_ptr<future<T>>(new future<T>(label));
	}
	static std::shared_ptr<future<T>> create_shared(
		const std::string &label = u8"unlabelled future"
	) {
		auto p = std::make_shared<future<T>>(label);
		p->shared(p);
		return p;
	}

	using checkpoint = std::chrono::high_resolution_clock::time_point;

	enum class state {
		pending,
		done,
		failed,
		cancelled
	};

	class fail_exception : public std::runtime_error {
	public:
		fail_exception(
			const std::string &msg
		):std::runtime_error{ msg }
		{
		}
	};

#if CAN_COPY_FUTURES
	/**
	 * Copy constructor with locking semantics.
	 */
	future(
		const future<T> &src
	):future(src, std::lock_guard<std::mutex>(src.mutex_))
	{
	}
#else
	/**
	 * Disable copy constructor. You can move a future, but copying it runs the
	 * risk of existing callbacks triggering more than once.
	 */
	future(
		const future<T> &src
	) = delete;
#endif

	/**
	 * Move constructor with locking semantics.
	 * @param src source future to move from
	 */
	future(
		future<T> &&src
	) noexcept
	 :future(
		std::move(src),
		std::lock_guard<std::mutex>(src.mutex_)
	 )
	{
	}

	/** Default constructor - nothing special here */
	future(
		const std::string &label = u8"unlabelled future"
	):state_(state::pending),
	  weak_ptr_(),
	  label_(label),
	  ex_(nullptr),
	  created_(std::chrono::high_resolution_clock::now())
	{
	}

	/**
	 * Default destructor too - virtual, in case anyone wants to subclass.
	 */
	virtual ~future() { }

	/** Returns the shared_ptr associated with this instance */
	std::shared_ptr<future<T>>
	shared()
	{
		/* Member function is from a dependent base so give the name lookup a nudge
		 * in the right (this->) direction
		 */
		std::lock_guard<std::mutex> guard { mutex_ };
		auto p = weak_ptr_.lock();
		if(p)
			return p;

		p = std::shared_ptr<future<T>>(this);
		weak_ptr_ = p;
		return p;
	}

	/** Returns the shared_ptr associated with this instance */
	std::shared_ptr<future<T>>
	shared(std::shared_ptr<future<T>> p)
	{
		/* Member function is from a dependent base so give the name lookup a nudge
		 * in the right (this->) direction
		 */
		std::lock_guard<std::mutex> guard { mutex_ };
		weak_ptr_ = std::move(p);
		return p;
	}

	/** Add a handler to be called when this future is marked as ready */
	std::shared_ptr<future<T>>
	on_ready(std::function<void(future<T> &)> code)
	{
		return call_when_ready(code);
	}

	/** Add a handler to be called when this future is marked as done */
	std::shared_ptr<future<T>>
	on_done(std::function<void(T)> code)
	{
		return call_when_ready([code](future<T> &f) {
			if(f.is_done()) {
				// std::cout << "will call value in ->on_Done handler\n";
				code(f.value());
			}
		});
	}

	/** Add a handler to be called if this future fails */
	std::shared_ptr<future<T>>
	on_fail(std::function<void(std::string)> code)
	{
		return call_when_ready([code](future<T> &f) {
			if(f.is_failed())
				code(f.failure_reason());
		});
	}

	/** Add a handler to be called if this future fails */
	template<typename E>
	std::shared_ptr<future<T>>
	on_fail(std::function<void(const E &)> code)
	{
		return call_when_ready([code](future<T> &f) {
			if(f.is_failed() && f.exception_ptr()) {
				try {
					std::rethrow_exception(f.exception_ptr());
				} catch(const T &e) {
					/* If our exception matches our expected type, handle it */
					code(e);
				} catch(...) {
					/* ... but skip any other exception types */
				}
			}
		});
	}

	/** Add a handler to be called if this future is cancelled */
	std::shared_ptr<future<T>> on_cancel(std::function<void(future<T> &)> code)
	{
		return call_when_ready([code](future<T> &f) {
			if(f.is_cancelled())
				code(f);
		});
	}

	/** Add a handler to be called if this future is cancelled */
	std::shared_ptr<future<T>> on_cancel(std::function<void()> code)
	{
		return call_when_ready([code](future<T> &f) {
			if(f.is_cancelled())
				code();
		});
	}

	/** Mark this future as done */
	std::shared_ptr<future<T>> done(T v)
	{
		return apply_state([v = std::move(v)](future<T>&f) {
			f.value_ = std::move(v);
		}, state::done);
	}

	/** Mark this future as failed */
	template<
		typename U,
		typename std::enable_if<
			is_string<U>::value,
			bool
		>::type * = nullptr
	>
	std::shared_ptr<future<T>>
	fail(
		const U ex
	)
	{
		// std::cout << "Calling string-handling fail(" << ex << ")\n";
		return fail(std::runtime_error(ex));
	}

	/**
	 * Mark this future as failed with the given exception.
	 */
	template<
		typename U,
		typename std::enable_if<
			!is_string<U>::value,
			bool
		>::type * = nullptr
	>
	std::shared_ptr<future<T>> fail(
		const U ex
	)
	{
		// std::cout << "Calling exception-handling fail(" << ex.what() << ")\n";
		return apply_state([&ex](future<T>&f) {
			try {
				throw ex;
			} catch(const std::exception &e) {
				// std::cout << "Will catch! " << e.what() << "\n";
				f.failure_reason_ = e.what();
				f.ex_ = std::current_exception();
			} catch(...) {
				// std::cout << "Will catch! (generic!)\n";
				f.failure_reason_ = "unknown";
				f.ex_ = std::current_exception();
			}
			// std::cout << "We're done!\n";
		}, state::failed);
	}

	template<typename U>
	std::shared_ptr<
		cps::future<T>
	>
	fail_from(const cps::future<U> &f) {
		// std::cout << "->fail_from with " << describe() << " taking info from " << f.describe() << "\n";
		if(!f.is_failed())
			throw std::logic_error("future is not failed");

		return apply_state([&f](future<T>&me) {
			try {
				// std::cout << "Will throw!\n";
				std::rethrow_exception(f.exception_ptr());
			} catch(const std::exception &e) {
				// std::cout << "Will catch!\n";
				me.ex_ = std::current_exception();
				me.failure_reason_ = e.what();
			} catch(...) {
				// std::cout << "Will catch!\n";
				me.ex_ = std::current_exception();
				me.failure_reason_ = "unknown";
			}
			// std::cout << "We're done!\n";
		}, state::failed);
	}

	/**
	 * Returns the current value for this future.
	 * Will throw a std::runtime_error if we're not marked as done.
	 */
	T value() const {
		// std::cout << "calling ->value on " << describe() << "\n";
		/* Only read this once */
		const state s { state_ };
		switch(s) {
		case state::pending:
			throw std::runtime_error("future is not complete");
		case state::failed:
#ifdef UNCAUGHT_EXCEPTION_DEBUGGING
			if(false && std::uncaught_exception()) {
				std::cerr << "Want to rethrow our exception, but we are already in an exception, so that's probably a bad idea\n";
				auto eptr = std::current_exception();
				try {
					std::rethrow_exception(eptr);
				} catch(const std::exception &e) {
					std::cerr << " - we were in a s::e as " << e.what() << "\n";
				} catch(const std::string &e) {
					std::cerr << " - we were in a string as " << e << "\n";
				} catch(const char *e) {
					std::cerr << " - we were in a char string as " << e << "\n";
				} catch(...) {
					std::cerr << " - we were in something else\n";
				}
				return value_;
			} else {
#endif
				if(ex_) {
					std::rethrow_exception(ex_);
				} else {
					throw std::logic_error("no exception available");
				}
#ifdef UNCAUGHT_EXCEPTION_DEBUGGING
			}
#endif
		case state::cancelled:
			throw std::runtime_error("future was cancelled");
		default:
			return value_;
		}
	}

	T value(std::error_code &ec) const {
		// std::cout << "calling ->value on " << describe() << "\n";
		/* Only read this once */
		const state s { state_ };
		switch(s) {
		case state::pending:
			ec = make_error_code(future_errc::is_pending);
			return T();
		case state::failed:
			ec = make_error_code(future_errc::is_failed);
			return T();
		case state::cancelled:
			ec = make_error_code(future_errc::is_cancelled);
			return T();
		default:
			return value_;
		}
	}

	template<
		typename U,
		typename V,
		typename std::enable_if<
			is_string<
				std::remove_pointer<
					decltype(
						arg_type_for(
							&V::operator()
						)
					)
				>
			>::value,
			bool
		>::type * = nullptr
	>
	auto
	exception_hoisting_callback(
		U ok,
		V code
	) -> std::function<decltype(ok(T()))(const std::exception_ptr &)>
	{
		using return_type = decltype(ok(T()));
		return [code](const std::exception_ptr &original) -> return_type {
			bool matched = false;
			std::string msg;
			try {
				std::rethrow_exception(original);
			} catch(const std::exception &e) {
				matched = true;
				msg = e.what();
			} catch(...) {
				matched = false;
			}
			return matched ? code(msg) : nullptr;
		};
	}

	/**
	 * Returns a callback that will run the given code if the exception
	 * is one that the code handles.
	 * Otherwise, the callback returns nullptr.
	 */
	template<
		typename U,
		typename V,
		typename std::enable_if<
			!is_string<
				std::remove_pointer<
					decltype(
						arg_type_for(
							&V::operator()
						)
					)
				>
			>::value,
			bool
		>::type * = nullptr
	>
	auto
	exception_hoisting_callback(
		U ok,
		V code
	) -> std::function<
		decltype(ok(T()))(const std::exception_ptr &)
	>
	{
		using return_type = decltype(ok(T()));
		typedef typename std::remove_pointer<decltype(arg_type_for(&V::operator()))>::type exception_type;
		return [code](const std::exception_ptr &original) -> return_type {
			try {
				std::rethrow_exception(original);
			} catch(const exception_type &e) {
				return code(e);
			} catch(...) {
			}
			return nullptr;
		};
	}

	/**
	 * This is one of the basic building blocks for composing futures, and
	 * is somewhat akin to an if/else statement.
	 *
	 * If the callback throws a std::exception, it will be caught and the
	 * returned future will be marked as failed with that exception.
	 *
	 * Callbacks will be passed either the current value, or the failure reason:
	 *
	 * * ok(this->value()) -> std::shared_ptr<future<X>>
	 * * err(this->failure_reason()) -> std::shared_ptr<future<X>>
	 *
	 * @param ok the function that will be called if this future resolves
	 * successfully. It is expected to return another future.
	 * @param err an optional function to call if this future fails. This
	 * is also expected to return a future.
	 * @returns a future of the same type as the ok callback will eventually
	 * return
	 */
	template<typename U, typename... Args>
	inline
	auto then(
		/* We trust this to be something that is vaguely callable, and that
		 * returns a shared_ptr-wrapped future. We use decltype and remove_reference
		 * to tear the opaque U type apart, thus guaranteeing (*) that we'll
		 * have a compilation failure if we get things wrong. Note that we
		 * can't pass a std::function<> here, at least not easily, because then
		 * the compiler is unable to deduce the function return type.
		 */
		U ok,
		Args... err
	) -> decltype(ok(T()))
	{
		/* We extract the type returned by the callback in stages, in a vain
		 * attempt to make this code easier to read
		 */

		/** The shared_ptr<future<X>> type */
		using future_ptr_type = decltype(ok(T()));
		/** The future<X> type */
		using future_type = typename std::remove_reference<decltype(*(future_ptr_type().get()))>::type;
		/** The X type, i.e. the type of the value held in the future */
		using inner_type = decltype(future_type().value());
		using return_type = decltype(ok(T()));

		/* This is what we'll return to the immediate caller: when the real future is
		 * available, we'll propagate the result onto f.
		 */
		auto f = future_type::create_shared();

		/* Gather the parameter pack by mapping the disparate types through our callback handler */
		std::vector<std::function<return_type(const std::exception_ptr &)>> items {
			exception_hoisting_callback(
				ok,
				err
			)...
		};

		call_when_ready([f, ok, items](future<T> &me) {
			/* Either callback could throw an exception. That's fine - it's even encouraged,
			 * since passing a future around to ->fail on is not likely to be much fun when
			 * dealing with external APIs.
			 */
			try {
				if(f->is_ready()) return;
				if(me.is_done()) {
					/* If we completed, call the function (exceptions will translate to f->fail)
					 * and set up propagation */
					// std::cout << "will call value in ->then handler for done status\n";
					auto inner = ok(me.value());
					inner->on_done([f](inner_type v) { f->done(v); })
						->on_fail([f, inner](const std::string &) { f->fail_from(*inner); })
						->on_cancel([f]() { f->cancel(); });
					/* TODO abandon vs. cancel */
					f->on_cancel([inner]() { inner->cancel(); });
				} else if(me.is_failed()) {
					/* The original future failed, so we try each exception handler in turn
					 * until we find one that matches. We'll stop after the first match.
					 */
					for(auto &it : items) {
						auto inner = it(me.ex_);
						if(inner) {
							inner->on_done([f](inner_type v) { f->done(v); })
								->on_fail([f, inner](const std::string &) { f->fail_from(*inner); })
								->on_cancel([f]() { f->cancel(); });
							/* TODO abandon vs. cancel */
							f->on_cancel([inner]() { inner->cancel(); });
							return;
						}
					}
					/* No handler was available, so we'll stick with the original failure */
					f->fail_from(me);
				} else if(me.is_cancelled()) {
					f->cancel();
				}
			} catch(...) {
				// std::cerr << "a wyld exception appears\n";
				auto ex = std::current_exception();
				f->fail_exception_pointer(ex);
			}
		});
		return f;
	}

	std::shared_ptr<cps::future<T>>
	fail_exception_pointer(const std::exception_ptr &ex)
	{
		return apply_state([&ex](future<T>&f) {
			f.ex_ = ex;
			try {
				std::rethrow_exception(ex);
			} catch(const std::exception &e) {
				f.failure_reason_ = e.what();
			} catch(...) {
				f.failure_reason_ = "unknown";
			}
		}, state::failed);
	}

	std::shared_ptr<future<T>>
	cancel() {
		return apply_state([](future<T>&) {
		}, state::cancelled);
	}

	/** Returns true if this future is ready (this includes cancelled, failed and done) */
	bool is_ready() const { return state_ != state::pending; }
	/** Returns true if this future completed successfully */
	bool is_done() const { return state_ == state::done; }
	/** Returns true if this future has failed */
	bool is_failed() const { return state_ == state::failed; }
	/** Returns true if this future is cancelled */
	bool is_cancelled() const { return state_ == state::cancelled; }
	/** Returns true if this future is not yet ready */
	bool is_pending() const { return state_ == state::pending; }

	/**
	 * Returns the failure reason (string) for this future.
	 * @throws std::runtime_error if we are not yet ready or didn't fail
	 */
	const std::string &failure_reason() const {
		if(state_ != state::failed)
			throw std::runtime_error("future is not failed");
		return failure_reason_;
	}

	/** Returns the label for this future */
	const std::string &label() const { return label_; }
	/** Returns the exception pointer */
	const std::exception_ptr &exception_ptr() const {
		if(state_ != state::failed)
			throw std::runtime_error("future is not failed");
		return ex_;
	}

	/**
	 * Reports number of nanoseconds that have elapsed so far
	 */
	std::chrono::nanoseconds elapsed() const {
		return (is_ready() ? resolved_ : std::chrono::high_resolution_clock::now()) - created_;
	}

	/**
	 * Returns the current future state, as a string.
	 */
	std::string current_state() const {
		return state_string(state_);
	}

	/**
	 * Returns a description for the given state.
	 */
	static std::string state_string(state s) {
		switch(s) {
		case state::pending: return u8"pending";
		case state::failed: return u8"failed";
		case state::cancelled: return u8"cancelled";
		case state::done: return u8"done";
		default: return u8"unknown";
		}
	}

	std::string time_string() const {
		using namespace std::chrono;
		auto e = elapsed();
		std::stringstream ss;
		auto d = duration_cast<duration<int, std::ratio<60*60*24>>>(e);
		if(d.count() != 0)
			ss << d.count() << "d";
		auto h = duration_cast<hours>(e -= d);
		if(h.count() != 0)
			ss << h.count() << "h";
		auto m = duration_cast<minutes>(e -= h);
		if(m.count() != 0)
			ss << m.count() << "m";
		auto s = duration_cast<seconds>(e -= m);
		if(s.count() != 0)
			ss << s.count() << "s";
		auto ms = duration_cast<milliseconds>(e -= s);
		if(ms.count() != 0)
			ss << ms.count() << "ms";
		auto us = duration_cast<microseconds>(e -= ms);
		if(us.count() != 0)
			ss << us.count() << u8"µs";
		auto ns = duration_cast<nanoseconds>(e -= us);
		if(ns.count() != 0)
			ss << ns.count() << u8"ns";
		return ss.str();
	}

	/**
	 * Returns a string description of the current test, of the form:
	 *
	 *     Future label (done), 14ms234ns
	 */
	std::string describe() const {
		return label_ + " (" + current_state() + "), " + time_string();
	}

protected:
	/**
	 * Queues the given function if we're not yet ready, otherwise
	 * calls it immediately. Will obtain a lock during the
	 * ready-or-queue check.
	 */
	std::shared_ptr<future<T>>
	call_when_ready(std::function<void(future<T> &)> code)
	{
		bool ready = false;
		{
			std::lock_guard<std::mutex> guard { mutex_ };
			ready = state_ != state::pending;
			if(!ready) {
				tasks_.push_back(code);
			}
		}
		if(ready) code(*this);
		return shared();
	}

	/**
	 * Removes an existing callback from the list, if we have one.
	 */
	std::shared_ptr<future<T>>
	remove_callback(std::function<void(future<T> &)> &code)
	{
		std::lock_guard<std::mutex> guard { mutex_ };
		tasks_.erase(
			std::remove_if(
				begin(tasks_),
				end(tasks_),
				[&code](const std::function<void(future<T> &)> &it) { return code == it; }
			)
		);
		return shared();
	}

	/**
	 * Runs the given code then updates the state.
	 */
	std::shared_ptr<future<T>> apply_state(std::function<void(future<T>&)> code, state s)
	{
		/* Cannot change state to pending, since we assume that we want
		 * to call all deferred tasks.
		 */
		assert(s != state::pending);

		std::vector<std::function<void(future<T> &)>> pending { };
		{
			std::lock_guard<std::mutex> guard { mutex_ };
			if(state_ != state::pending)
				throw std::logic_error("tried to resolve future twice, wanted " + state_string(s) + ":" + describe());

			code(*this);
			pending = std::move(tasks_);
			tasks_.clear();
			/* This must happen last */

			resolved_ = std::chrono::high_resolution_clock::now();
			state_ = s;
			/* Might want to consider something like compare_exchange_strong(...) if we need a mutex-free version in future:? */
		}
		for(auto &v : pending) {
			v(*this);
		}
		return shared();
	}

//#if CAN_COPY_FUTURES
	/**
	 * Locked constructor for internal use.
	 * Copies from the source instance, protected by the given mutex.
	 */
	future(
		const future<T> &src,
		const std::lock_guard<std::mutex> &
	):state_(src.state_.load()),
	  weak_ptr_(src.weak_ptr_),
	  tasks_(src.tasks_),
	  ex_(src.ex_),
	  label_(src.label_),
	  created_(src.created_),
	  resolved_(src.resolved_),
	  value_(src.value_)
	{
	}
//#endif

	/**
	 * Locked move constructor, for internal use.
	 */
	future(
		future<T> &&src,
		const std::lock_guard<std::mutex> &
	) noexcept
	 :state_(std::move(src.state_)),
	  weak_ptr_(std::move(src.weak_ptr_)),
	  tasks_(std::move(src.tasks_)),
	  ex_(src.ex_),
	  label_(std::move(src.label_)),
	  created_(std::move(src.created_)),
	  resolved_(std::move(src.resolved_)),
	  value_(std::move(src.value_))
	{
	}

protected:
	/** Guard variable for serialising updates to the tasks_ member */
	mutable std::mutex mutex_;
	/** Current future state. Atomic so we can get+set from multiple threads without needing a full lock */
	std::atomic<state> state_;
	/** Track current shared_ptr, for cases where we act as a shared_ptr (i.e. most of the time) */
	mutable std::weak_ptr<future<T>> weak_ptr_;
	/** The list of tasks to run when we are resolved */
	std::vector<std::function<void(future<T> &)>> tasks_;
	/** The final value of the future, if we completed successfully */
	T value_;
	/** The exception as a string, if we failed */
	std::string failure_reason_;
	/** The exception, if we failed */
	std::exception_ptr ex_;
	/** Label for this future */
	std::string label_;
	/** When we were created */
	checkpoint created_;
	/** When we were marked ready */
	checkpoint resolved_;
};

template<
	typename T
>
std::shared_ptr<future<T>>
resolved_future(T v)
{
	return future<T>::create_shared()->done(v);
}

template<
	typename T
>
std::shared_ptr<future<T>>
make_future()
{
	return future<T>::create_shared();
}

template<
	typename T
>
std::shared_ptr<future<T>>
make_future(const std::string &label)
{
	return future<T>::create_shared(label);
}

};

