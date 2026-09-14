// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <kernel/thread/thread_data_queue.h>
#include <kernel/types.h>
#include <util/byte_ring_buffer.h>

struct KernelState;

struct WaitingThreadData {
    ThreadStatePtr thread;
    int32_t priority;
    bool *was_canceled;

    // additional fields for each primitive
    union {
        struct { // mutex
            int32_t lock_count;
        };
        struct { // rwlock
            bool is_write;
        };
        struct { // semaphore
            int32_t signal;
        };
        struct { // simple events
            int32_t pattern;
            uint32_t *result_pattern;
            uint64_t *user_data;
        };
        struct { // event flags
            int32_t wait;
            int32_t flags;
            uint32_t *outBits;
        };
        // struct { }; // condvar
        struct { // msgpipe
            SceSize request_size;
        } mp;
    };

    bool operator<(const WaitingThreadData &rhs) const {
        return priority < rhs.priority;
    }

    bool operator>(const WaitingThreadData &rhs) const {
        return priority > rhs.priority;
    }

    bool operator==(const WaitingThreadData &rhs) const {
        return thread == rhs.thread;
    }

    bool operator==(const ThreadStatePtr &rhs) const {
        return thread == rhs;
    }
};

using WaitingThreadQueuePtr = std::unique_ptr<ThreadDataQueue<WaitingThreadData>>;

// NOTE: uid is copied to sync primitives here for debugging,
//       not really needed since they are put in std::map's
struct SyncPrimitive {
    SceUID uid{};
    uint32_t attr{};
    std::mutex mutex;
    char name[KERNELOBJECT_MAX_NAME_LENGTH + 1];
    virtual ~SyncPrimitive() = default;
};

struct SimpleEvent : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    SceUInt32 pattern;
    SceUInt64 last_user_data;

    bool auto_reset;
    bool cb_wakeup_only;
};

using SimpleEventPtr = std::shared_ptr<SimpleEvent>;
using SimpleEventPtrs = std::map<SceUID, SimpleEventPtr>;

struct Timer : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    std::condition_variable condvar;

    bool is_started = false;
    bool is_repeat = false;
    bool is_pulse = false;
    bool event_set = false;
    uint64_t time = 0;
    uint64_t next_event;
    uint64_t event_interval = 0;
};

using TimerPtr = std::shared_ptr<Timer>;
using TimerPtrs = std::map<SceUID, TimerPtr>;

struct Semaphore : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    int max;
    int val;
    int init_val;
};

using SemaphorePtr = std::shared_ptr<Semaphore>;
using SemaphorePtrs = std::map<SceUID, SemaphorePtr>;

struct Mutex : SyncPrimitive {
    int init_count;
    int lock_count;
    ThreadStatePtr owner;
    WaitingThreadQueuePtr waiting_threads;
    Ptr<SceKernelLwMutexWork> workarea;
};

using MutexPtr = std::shared_ptr<Mutex>;
using MutexPtrs = std::map<SceUID, MutexPtr>;

enum class RWLockState {
    Unlocked,
    ReadLocked,
    WriteLocked,
};

// the int value is the lock count for recursive locks
using RWLockOwners = std::map<ThreadStatePtr, int>;

struct RWLock : SyncPrimitive {
    RWLockState state;
    RWLockOwners owners;
    WaitingThreadQueuePtr waiting_threads;
};

using RWLockPtr = std::shared_ptr<RWLock>;
using RWLockPtrs = std::map<SceUID, RWLockPtr>;

struct EventFlag : SyncPrimitive {
    WaitingThreadQueuePtr waiting_threads;
    int flags;
};

using EventFlagPtr = std::shared_ptr<EventFlag>;
using EventFlagPtrs = std::map<SceUID, EventFlagPtr>;

struct Condvar : SyncPrimitive {
    struct SignalTarget {
        enum class Type {
            Any, // signal any one waiting thread
            Specific, // signal a specific waiting thread (target_thread)
            All, // signal all waiting threads
        } type;

        SceUID thread_id; // for Type::One

        explicit SignalTarget(Type type)
            : type(type)
            , thread_id(0) {}
        SignalTarget(Type type, SceUID thread_id)
            : type(type)
            , thread_id(thread_id) {}
    };

    WaitingThreadQueuePtr waiting_threads;
    MutexPtr associated_mutex;
};
using CondvarPtr = std::shared_ptr<Condvar>;
using CondvarPtrs = std::map<SceUID, CondvarPtr>;

struct MsgPipe : SyncPrimitive {
    MsgPipe(std::size_t bufSize)
        : data_buffer(bufSize) {}

    WaitingThreadQueuePtr senders;
    WaitingThreadQueuePtr receivers;
    ByteRingBuffer data_buffer;

    bool beingDeleted = false;
    std::atomic<std::size_t> remainingThreads = { 0 };

    ~MsgPipe() override = default;
};

using MsgPipePtr = std::shared_ptr<MsgPipe>;
using MsgPipePtrs = std::map<SceUID, MsgPipePtr>;

enum class SyncWeight {
    Light, // lightweight
    Heavy // 'heavy'weight
};

// simple events
SceUID simple_event_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr, SceUInt32 init_pattern);
SceInt32 simple_event_waitorpoll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 wait_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout, bool is_wait);
SceInt32 simple_event_setorpulse(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 pattern, SceUInt64 user_data, bool is_set);
SceInt32 simple_event_clear(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 clear_pattern);
SceInt32 simple_event_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id);

// Timer
SceUID timer_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr);
SceUID timer_find(KernelState &kernel, const char *export_name, const char *pName);
SceInt32 timer_waitorpoll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 bit_pattern, SceUInt32 *result_pattern, SceUInt64 *user_data, SceUInt32 *timeout, bool is_wait);
SceInt32 timer_clear(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 clear_pattern);
SceInt32 timer_set(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle, SceUID type, SceKernelSysClock *interval, SceInt32 repeats);
SceInt32 timer_start(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle);
SceInt32 timer_stop(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID timer_handle);

// Mutex
SceUID mutex_create(SceUID *uid_out, KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, int init_count, Ptr<SceKernelLwMutexWork> workarea, SyncWeight weight);
SceUID mutex_find(KernelState &kernel, const char *export_name, const char *pName);
int mutex_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID mutexid, int lock_count, unsigned int *timeout, SyncWeight weight);
int mutex_try_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID mutexid, int lock_count, SyncWeight weight);
int mutex_unlock(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, int unlock_count, SyncWeight weight);
int mutex_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, SyncWeight weight);
MutexPtr mutex_get(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID mutexid, SyncWeight weight);

// RWLock
SceUID rwlock_create(KernelState &kernel, MemState &mem, const char *export_name, const char *name, SceUID thread_id, SceUInt32 attr);
SceInt32 rwlock_lock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id, uint32_t *timeout, bool is_write);
SceInt32 rwlock_unlock(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id, bool is_write);
SceInt32 rwlock_delete(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID lock_id);

// Semaphore
SceUID semaphore_create(KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, int init_val, int max_val);
SceUID semaphore_find(KernelState &kernel, const char *export_name, const char *pName);
SceInt32 semaphore_wait(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaId, SceInt32 needCount, SceUInt32 *pTimeout);
int semaphore_signal(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid, int signal);
int semaphore_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid);
int semaphore_cancel(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID semaid, SceInt32 setCount, SceUInt32 *pNumWaitThreads);

// Condition Variable
SceUID condvar_create(SceUID *uid_out, KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, SceUID assoc_mutexid, SyncWeight weight);
int condvar_wait(KernelState &kernel, MemState &mem, const char *export_name, SceUID thread_id, SceUID condid, SceUInt *timeout, SyncWeight weight);
int condvar_signal(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID condid, Condvar::SignalTarget signal_target, SyncWeight weight);
int condvar_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID condid, SyncWeight weight);

// Event Flag
SceUID eventflag_clear(KernelState &kernel, const char *export_name, SceUID evfId, SceUInt32 bitPattern);
SceUID eventflag_create(KernelState &kernel, const char *export_name, SceUID thread_id, const char *pName, SceUInt32 attr, SceUInt32 initPattern);
SceUID eventflag_find(KernelState &kernel, const char *export_name, const char *pName);
SceInt32 eventflag_wait(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID evfId, SceUInt32 bitPattern, SceUInt32 waitMode, SceUInt32 *pResultPat, SceUInt32 *pTimeout);
int eventflag_poll(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, unsigned int flags, unsigned int wait, unsigned int *outBits);
SceInt32 eventflag_set(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID evfId, SceUInt32 bitPattern);
SceInt32 eventflag_cancel(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id, SceUInt32 pattern, SceUInt32 *num_wait_threads);
int eventflag_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID event_id);

// Message Pipe
SceUID msgpipe_create(KernelState &kernel, const char *export_name, const char *name, SceUID thread_id, SceUInt attr, SceSize bufSize);
SceUID msgpipe_find(KernelState &kernel, const char *export_name, const char *pName);
SceSize msgpipe_recv(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgPipeId, SceUInt32 waitMode, void *pRecvBuf, SceSize recvSize, SceUInt32 *pTimeout);
SceSize msgpipe_send(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgPipeId, SceUInt32 waitMode, const void *pSendBuf, SceSize sendSize, SceUInt32 *pTimeout);
SceInt32 msgpipe_delete(KernelState &kernel, const char *export_name, SceUID thread_id, SceUID msgpipe_id);
