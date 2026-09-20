// A real, recursive mutex, because this module genuinely has two threads.
//
// ScummVM ships NullMutexInternal, a no-op, for backends that are single
// threaded. This one is not: the game runs on its own thread while the host
// calls trace_update() on another to pump audio and saves. ScummVM's mixer
// relies on the OSystem mutex to guard exactly that boundary, so a no-op mutex
// is a data race -- it runs, draws a couple of hundred frames, and wedges as
// soon as audio starts mixing.
//
// 🚨 The recursion is counted by hand on purpose. wasi-libc DEFINES
// PTHREAD_MUTEX_RECURSIVE but does not implement pthread_mutexattr_settype, so
// asking for a recursive mutex there silently gives you a plain one -- and
// ScummVM takes this mutex again on the same thread (the mixer does), which
// then deadlocks before the engine draws anything at all.
#ifndef TRACE_MUTEX_H
#define TRACE_MUTEX_H

#include <pthread.h>

#define FORBIDDEN_SYMBOL_EXCEPTION_time_h

#include "common/mutex.h"
#include "trace_stage.h"

class TraceMutexInternal final : public Common::MutexInternal {
public:
	TraceMutexInternal() {
		trace_stage("mutex: pthread_mutex_init");
		pthread_mutex_init(&_m, nullptr);
		trace_stage("mutex: inited");
	}
	~TraceMutexInternal() override { pthread_mutex_destroy(&_m); }

	bool lock() override {
		const pthread_t self = pthread_self();
		if (_depth > 0 && pthread_equal(_owner, self)) {
			_depth++;                     // already ours: just go deeper
			return true;
		}
		trace_stage("mutex: locking");
		if (pthread_mutex_lock(&_m) != 0) return false;
		trace_stage("mutex: locked");
		_owner = self;
		_depth = 1;
		return true;
	}

	bool unlock() override {
		if (_depth == 0) return false;    // not held; nothing to do
		if (--_depth > 0) return true;    // still held further up the stack
		_owner = pthread_t();
		const bool ok = pthread_mutex_unlock(&_m) == 0;
		trace_stage("mutex: unlocked");
		return ok;
	}

private:
	pthread_mutex_t _m;
	// Only ever read or written while the mutex is held, or by the thread that
	// already owns it, so these need no atomics of their own.
	pthread_t _owner = pthread_t();
	int _depth = 0;
};

#endif
