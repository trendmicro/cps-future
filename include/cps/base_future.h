#pragma once
#include <memory>
#include <atomic>

#include <string>

#if FUTURE_TRACE
#include <iostream>
#include <iomanip>
#include <typeinfo>
#include <cxxabi.h>

#include <boost/log/trivial.hpp>

#define TRACE BOOST_LOG_TRIVIAL(trace)
#endif

#if FUTURE_TIMERS
#include <boost/chrono/chrono_io.hpp>
#endif // FUTURE_TIMERS

#include <vector>

namespace cps {

/**
 * Deferred result handling.
 * This class provides the base implementation for the "future" deferred-task concept.
 */
class base_future : public std::enable_shared_from_this<base_future> {
#if FUTURE_TIMERS
private:
	typedef boost::chrono::high_resolution_clock::time_point checkpoint;
#endif
public:
	/** A std::shared_ptr to a cps::base_future */
	typedef std::shared_ptr<base_future> ptr;
	typedef std::function<ptr()> seq;

	/** Current state of this future */
	enum state {
		pending,
		cancelled,
		failed,
		complete
	};

	/** Holds information about a failure */
	class exception {
	public:
		exception(
			std::shared_ptr<std::exception> e,
			const std::string &component
		):ex_(),
		  component_(component),
		  reason_(u8"unknown")
		{
		}

		virtual ~exception() { }

		std::exception &ex() const { return *ex_; }
		const std::string &reason() const { return reason_; }

	private:
		std::shared_ptr<std::exception> ex_;
		std::string component_;
		std::string reason_;
	};

	class ready_exception : public std::runtime_error {
	public:
		ready_exception(
		):std::runtime_error{ "cps::future is not ready" }
		{
		}
	};

	class cancel_exception : public std::runtime_error {
	public:
		cancel_exception(
		):std::runtime_error{ "cps::future is cancelled" }
		{
		}
	};

	class fail_exception : public std::runtime_error {
	public:
		fail_exception(
			const std::string &msg
		):std::runtime_error{ msg }
		{
		}
	};

	/** Instantiates a new future with the given label */
	base_future(
		const std::string &label = u8"unlabelled future"
	):state_{pending},
	  label_(label)
#if FUTURE_TIMERS
	 ,created_(boost::chrono::high_resolution_clock::now())
#endif // FUTURE_TIMERS
	{
#if FUTURE_TRACE
		TRACE << " future(" << label_ << ")";
#endif
	}

virtual ~base_future() {
#if FUTURE_TRACE
		TRACE << "~base_future(" << label_ << ") " << describe_state();
#endif
	}

	/**
	 * Reports the state of this base_future.
	 * String returned will be one of:
	 * <ul>
	 * <li>pending
	 * <li>cancelled
	 * <li>failed
	 * <li>complete
	 * </ul>
	 * If something has gone badly wrong, this will report unknown with the actual numerical
	 * state in (). This usually indicates memory corruption or a deleted object.
	 */
	std::string
	describe_state() const
	{
		switch(state_) {
		case pending: return "pending";
		case cancelled: return "cancelled";
		case failed: return "failed";
		case complete: return "complete";
		default: return "unknown (" + std::to_string((int)state_) + ")";
		}
	}

	/**
	 * Creates a new shared_ptr<base_future>.
	 */
	static
	ptr
	create() {
		struct accessor : public base_future { };
		return std::make_shared<accessor>();
	}

	/**
	 * Marks this base_future as done.
	 * Subclasses should override this to accept a value.
	 */
	ptr done() {
		auto self = shared_from_this();
#if FUTURE_TRACE
		TRACE << " ->done() was " << describe_state() << " on " << label_;
#endif
		mark_ready(complete);
		on_fail_.clear();
		on_cancel_.clear();
		while(!on_done_.empty()) {
			auto copy = on_done_;
			on_done_.clear();
			for(auto &it : copy) {
#if FUTURE_TRACE
				TRACE << "trying handler " << (void*)(&it) << " on " << label_;
#endif
				try {
					(it)();
				} catch(std::string ex) {
					std::cerr << "Exception in callback - " << ex;
				} catch(...) {
					std::cerr << "Unknown exception in callback";
					throw;
				}
			}
#if FUTURE_TRACE
			TRACE << "finished handler as " << describe_state() << " with " << on_done_.size() << " remaining" << " on " << label_;
#endif
		}
		return self;
	}

	/**
	 * Marks this base_future as failed.
	 */
	ptr fail(
		exception &ex
	) {
		ex_ = std::unique_ptr<exception>(new exception(ex));
		return fail();
	}

	/**
	 * Marks this base_future as failed.
	 */
	ptr fail(
		std::shared_ptr<std::exception> ex,
		const std::string &component = u8"unknown"
	) {
		ex_ = std::unique_ptr<exception>(
			new exception(
			ex,
			component
			)
		);
		return fail();
	}

	/**
	 * Marks this base_future as failed.
	 */
	ptr fail(
		const std::string &ex,
		const std::string &component = u8"unknown"
	) {
		ex_ = std::unique_ptr<exception>(
			new exception(
				std::make_shared<fail_exception>(ex),
				component
			)
		);
		return fail();
	}

	/**
	 * Adds code to the list of things that should be called when this base_future
	 * resolves, regardless of state.
	 */
	ptr on_ready(std::function<void(ptr)> code) {
		auto self = shared_from_this();
		auto handler = [self, code]() { code(self); };
		on_done(handler);
		on_cancel(handler);
		auto fail_handler = [self, code](exception &) { code(self); };
		on_fail(fail_handler);
		return self;
	}

	/**
	 * Attaches this base_future to another base_future, so that we get resolved with
	 * the same state as the other base_future.
	 */
	ptr propagate(ptr f) {
#if FUTURE_TRACE
		TRACE << "propagating " << f->describe_state() << " from " << describe_state() << " on " << label_;
#endif
		on_done([f]() {
			f->done();
		});
		on_cancel([f]() {
			f->cancel();
		});
		on_fail([f](exception &e) {
			f->fail(e);
		});
		return f;
	}

	static
	base_future::ptr
	repeat(std::function<bool(base_future::ptr)> check, std::function<base_future::ptr(base_future::ptr)> each) {
		auto f = create();
		// Keep f around until it's finished
		f->on_ready([f](ptr in) { });

		auto next = create();
		next->done();
		std::shared_ptr<std::function<base_future::ptr(base_future::ptr)>> code = std::make_shared<std::function<base_future::ptr(base_future::ptr)>>([f, check, code, each] (base_future::ptr in) mutable -> base_future::ptr {
#if FUTURE_TRACE
			TRACE << "Entering code() with " << (void*)(&(*code));
#endif
			std::function<base_future::ptr(base_future::ptr)> recurse;
			recurse = [&,f](base_future::ptr in) -> base_future::ptr {
#if FUTURE_TRACE
				TRACE << "Entering recursion with " << f->describe_state() << " on " << f->label_;
#endif
				if(f->is_ready()) return f;

				{
					bool status = check(in);
#if FUTURE_TRACE
					TRACE << "Check returns " << status;
#endif
					if(status) {
#if FUTURE_TRACE
						TRACE << "And we are done";
#endif
						return f->done();
					}
				}

				in = each(in);
				return in->then([f, in, &recurse]() -> base_future::ptr {
#if FUTURE_TRACE
					TRACE << "each then with f = " << f->describe_state() << " and in = " << in->describe_state() << " on " << f->label_;
#endif
					auto v = recurse(in);
#if FUTURE_TRACE
					TRACE << "v was " << v << " on " << f->label_;
#endif
					return v;
				})->on_fail([f](exception &ex) {
					f->fail(ex);
				})->on_cancel([f]() {
					f->fail("cancelled");
				});
			};
			return recurse(in);
		});
#if FUTURE_TRACE
		TRACE << "Calling next";
#endif
		next = (*code)(next);
		f->on_ready([code](base_future::ptr) -> base_future::ptr { return create()->done(); });
		return f;
	}

	static ptr needs_all(std::vector<ptr> pending) {
		auto f = create();
		auto count = std::make_shared<std::atomic<int>>();
		auto p = std::make_shared<std::vector<ptr>>(std::move(pending));
		*count = pending.size();
		auto h = [f, count]() {
#if FUTURE_TRACE
			TRACE << "as " << f->describe_state() << " with count = " << *count << " on " << f->label_;
#endif
			if(0 == --*count && !f->is_ready()) f->done();
#if FUTURE_TRACE
			TRACE << "now " << f->describe_state() << " with count = " << *count << " on " << f->label_;
#endif
		};
		auto ok = [f, h]() {
			if(f->is_ready()) return;
			h();
		};
		auto fail_handler = [h, p, f](exception &ex) {
			if(f->is_ready()) return;
			for(auto &it : *p) {
				if(!it->is_ready()) it->cancel();
			}
			f->fail(ex);
		};
		auto can = [h, p, f]() {
			if(f->is_ready()) return;

			for(auto &it : *p) {
				if(!it->is_ready()) it->cancel();
			}
			f->cancel();
		};
		for(auto &it : *p) {
			it->on_done(ok);
			it->on_fail(fail_handler);
			it->on_cancel(can);
		}
		return f;
	}

private:

	ptr fail() {
		auto self = shared_from_this();
		mark_ready(failed);
		on_done_.clear();
		on_cancel_.clear();
		while(!on_fail_.empty()) {
			auto copy = std::move(on_fail_);
			on_fail_.clear();
			for(auto &it : copy) {
				try {
					(it)(*ex_);
				} catch(std::string ex) {
					std::cerr << "Exception in callback - " << ex;
				} catch(...) {
					std::cerr << "Unknown exception in callback";
					throw;
				}
			}
		}
		return self;
	}

public:
	ptr cancel() {
		auto self = shared_from_this();
		mark_ready(cancelled);
		on_done_.clear();
		on_fail_.clear();
		while(!on_cancel_.empty()) {
			auto copy = std::move(on_cancel_);
			on_cancel_.clear();
			for(auto &it : copy) {
				try {
					(it)();
				} catch(std::string ex) {
					std::cerr << "Exception in callback - " << ex;
				} catch(...) {
					std::cerr << "Unknown exception in callback";
					throw;
				}
			}
		}
	}

	static ptr complete_base_future() { auto f = create(); f->done(); return f; }

	ptr then(seq ok) {
		auto self = shared_from_this();
		auto f = create();
		on_done([self, ok, f]() {
#if FUTURE_TRACE
			TRACE << "Marking me as done" << " on " << self->label_;
#endif
			if(f->is_ready()) return;
			auto s = ok();
			s->propagate(f);
		});
		on_fail([self, f](exception &ex) {
#if FUTURE_TRACE
			TRACE << "Marking me as failed" << " on " << self->label_;
#endif
			if(f->is_ready()) return;
			f->fail(ex);
		});
		on_cancel([self, f]() {
#if FUTURE_TRACE
			TRACE << "Marking me as cancelled" << " on " << self->label_;
#endif
			if(f->is_ready()) return;
			f->cancel();
		});
		return f;
	}

	ptr on_done(std::function<void()> code) {
		auto self = shared_from_this();
		if(!is_ready()) {
			on_done_.push_back(code);
			return self;
		} 
		if(is_done()) code();
		return self;
	}

	ptr on_cancel(std::function<void()> code) {
		auto self = shared_from_this();
		if(!is_ready()) {
			on_cancel_.push_back(code);
			return self;
		} 
		if(is_cancelled()) code();
		return self;
	}

	ptr on_fail(std::function<void(exception&)> code) {
		auto self = shared_from_this();
		if(!is_ready()) {
			on_fail_.push_back(code);
			return self;
		} 
		if(is_failed()) code(*ex_);
		return self;
	}

	ptr then(std::function<ptr()> ok, std::function<ptr(exception&)> fail) {
		auto f = create();
		if(ok != nullptr) {
			then_.push_back(ok);
		}
		else_.push_back(fail);
		return f;
	}

	bool is_pending() const { return state_ == state::pending; }
	bool is_ready() const { return state_ != state::pending; }
	bool is_failed() const { return state_ == state::failed; }
	bool is_done() const { return state_ == state::complete; }
	bool is_cancelled() const { return state_ == state::cancelled; }
	std::string failure() const {
		if(!is_failed())
			throw fail_exception(u8"this base_future is not failed");
		return ex_->reason();
	}

protected:
	/** Used internally to mark this base_future as ready. Takes a single parameter
	 * which indicates the state - failed, completed, cancelled */
	void mark_ready(state s) {
		state_ = s;
#if FUTURE_TIMERS
		resolved_ = boost::chrono::high_resolution_clock::now();
#endif // FUTURE_TIMERS
	}

protected:
	std::atomic<state> state_;
	std::string label_;

#if FUTURE_TIMERS
	checkpoint created_;
	checkpoint resolved_;
#endif // FUTURE_TIMERS

	std::vector<std::function<ptr()>> then_;
	std::vector<std::function<ptr(exception &)>> else_;
	std::vector<std::function<void()>> on_done_;
	std::vector<std::function<void(exception &)>> on_fail_;
	std::vector<std::function<void()>> on_cancel_;

	std::unique_ptr<exception> ex_;
};

};