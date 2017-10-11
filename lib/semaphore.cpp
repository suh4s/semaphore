/*

Copyright (c) 2017, NVIDIA Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef __semaphore_cuda

#include "details/config.hpp"

namespace {

#ifdef __semaphore_fast_path

#ifdef __linux__
// On Linux, we make use of the kernel memory wait operations. These have been available for a long time.
template <class A>
inline const void *__semaphore_fixalign(A &a)
{
    static_assert(sizeof(A) <= sizeof(int), "Linux only supports 'int' for Futex.");
    return (const void *)((intptr_t)&a & ~(sizeof(int) - 1));
}
inline int __semaphore_readint(const void *p)
{
    int i;
    memcpy(&i, p, sizeof(int));
    return i;
}
template <class A, class V>
inline void __semaphore_wait(A &a, V v)
{
    auto p = __semaphore_fixalign(a);
    auto i = __semaphore_readint(p);
    asm volatile("" ::
                     : "memory");
    if (a.load(std::memory_order_relaxed) != v)
        return;
    syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, i, 0, 0, 0);
}
template <class A, class V, class Rep, class Period>
void __semaphore_wait_timed(A &a, V v, const std::chrono::duration<Rep, Period> &t)
{
    auto p = __semaphore_fixalign(a);
    auto i = __semaphore_readint(p);
    asm volatile("" ::
                     : "memory");
    if (a.load(std::memory_order_relaxed) != v)
        return;
    syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, i, __semaphore_to_timespec(t), 0, 0);
}
template <class A>
inline void __semaphore_wake_one(A &a)
{
    syscall(SYS_futex, __semaphore_fixalign(a), FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
}
template <class A>
inline void __semaphore_wake_all(A &a)
{
    syscall(SYS_futex, __semaphore_fixalign(a), FUTEX_WAKE_PRIVATE, INT_MAX, 0, 0, 0);
}
template <class A, class V>
inline void __semaphore_wait(volatile A &a, V v)
{
    auto p = __semaphore_fixalign(a);
    auto i = __semaphore_readint(p);
    asm volatile("" ::
                     : "memory");
    if (a.load(std::memory_order_relaxed) != v)
        return;
    syscall(SYS_futex, p, FUTEX_WAIT, i, 0, 0, 0);
}
template <class A, class V, class Rep, class Period>
void __semaphore_wait_timed(volatile A &a, V v, const std::chrono::duration<Rep, Period> &t)
{
    auto p = __semaphore_fixalign(a);
    auto i = __semaphore_readint(p);
    asm volatile("" ::
                     : "memory");
    if (a.load(std::memory_order_relaxed) != v)
        return;
    syscall(SYS_futex, p, FUTEX_WAIT, i, __semaphore_to_timespec(t), 0, 0);
}
template <class A>
inline void __semaphore_wake_one(volatile A &a)
{
    syscall(SYS_futex, __semaphore_fixalign(a), FUTEX_WAKE, 1, 0, 0, 0);
}
template <class A>
inline void __semaphore_wake_all(volatile A &a)
{
    syscall(SYS_futex, __semaphore_fixalign(a), FUTEX_WAKE, INT_MAX, 0, 0, 0);
}
#endif // __linux__

#if defined(WIN32)
// On Windows, we make use of the kernel memory wait operations as well. These first became available with Windows 8.
template <class A, class V>
void __semaphore_wait(A &a, V v)
{
    static_assert(sizeof(V) <= 8, "Windows only allows sizes between 1B and 8B for WaitOnAddress.");
    WaitOnAddress((PVOID)&a, (PVOID)&v, sizeof(v), -1);
}
template <class A, class V, class Rep, class Period>
void __semaphore_wait_timed(A &a, V v, std::chrono::duration<Rep, Period> const &delta)
{
    static_assert(sizeof(V) <= 8, "Windows only allows sizes between 1B and 8B for WaitOnAddress.");
    WaitOnAddress((PVOID)&a, (PVOID)&v, sizeof(v), (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}
template <class A>
inline void __semaphore_wake_one(A &a)
{
    WakeByAddressSingle((PVOID)&a);
}
template <class A>
inline void __semaphore_wake_all(A &a)
{
    WakeByAddressAll((PVOID)&a);
}
#endif // defined(WIN32)

#endif //__semaphore_fast_path

} //anonymous

#endif // __semaphore_cuda

#include "semaphore"

namespace __semaphore_ns
{
namespace experimental
{
inline namespace v1 
{
namespace details
{

template<class Fn>
__semaphore_abi bool __binary_semaphore_acquire_slow(
    atomic<binary_semaphore::count_type>& atom, atomic<binary_semaphore::count_type>& ticket,
    atomic<binary_semaphore::count_type>& tocket, bool const& stolen, Fn fn)
{
    uint32_t const tick = ticket.fetch_add(1, std::memory_order_relaxed);
    uint32_t tock = tocket.load(std::memory_order_relaxed);
    uint32_t contbit = 0u;
#ifdef __semaphore_fast_path
    uint32_t sum = 0u;
    while(1) {
        if(sum < 64*1024) {
#else //__semaphore_fast_path
    while(1) {
#endif //__semaphore_fast_path
            uint32_t const delta = (tick - tock) * 128;
#if !defined(__CUDA_ARCH__)
            std::this_thread::sleep_for(std::chrono::nanoseconds(delta));
#elif defined(__has_cuda_nanosleep)
            details::__mme_nanosleep(delta);
#endif //__CUDA_ARCH__
#ifdef __semaphore_fast_path
            sum += delta;
        }
        else 
        {
            uint32_t old = atom.fetch_or(binary_semaphore::__slowbit, std::memory_order_relaxed) | binary_semaphore::__slowbit;
            if ((old & binary_semaphore::__valubit) != 0) {
                atomic_thread_fence(std::memory_order_seq_cst);
                if(!fn(old))
                    return false;
            }
        }
        uint32_t old = atom.load(std::memory_order_relaxed);
#else //__semaphore_fast_path
        uint32_t old = atom.load(std::memory_order_relaxed);
        if(!fn(old))
            return false;
#endif //__semaphore_fast_path
        tock = tocket.load(std::memory_order_relaxed);
        if(tock != tick)
            continue;
        while ((old & binary_semaphore::__valubit) == 0) {
            old &= ~binary_semaphore::__lockbit;
            uint32_t next = old - contbit + binary_semaphore::__valubit;
            if (atom.compare_exchange_weak(old, next, std::memory_order_acquire, std::memory_order_relaxed))
                return true;
        }
        if(contbit == 0)
            atom.fetch_add(contbit = binary_semaphore::__contbit, std::memory_order_relaxed);
    }
}

} //details

#ifdef __semaphore_fast_path
__semaphore_abi void binary_semaphore::__release_slow(count_type old)
{
    count_type lock = 0;
    do {
        old &= ~__lockbit;
        lock = (old & __slowbit) ? __lockbit : 0;
    } while (!__atom.compare_exchange_weak(old, (old | lock) & ~(__valubit | __slowbit), std::memory_order_release, std::memory_order_relaxed));
    if (lock != 0)
    {
        atomic_thread_fence(std::memory_order_seq_cst);
        __semaphore_wake_all(__atom);
        __atom.fetch_and(~__lockbit, std::memory_order_release);
    }
}
#endif //__semaphore_fast_path

__semaphore_abi void binary_semaphore::__acquire_slow()
{
    auto const fn = [=] __semaphore_abi (uint32_t old) -> bool { 
#ifdef __semaphore_fast_path
        __semaphore_wait(__atom, old); 
#endif //__semaphore_fast_path
        return true;
    };
    details::__binary_semaphore_acquire_slow(__atom, __ticket, __tocket, __stolen, fn);
}

#ifndef __semaphore_cuda

__semaphore_abi bool binary_semaphore::__acquire_slow_timed(std::chrono::nanoseconds const& rel_time) 
{
    auto const fn = [=](uint32_t old) __semaphore_abi -> bool { 
        auto const abs_time = details::__semaphore_clock::now() + rel_time;
#ifdef __semaphore_fast_path
        if(rel_time > std::chrono::microseconds(0))
            __semaphore_wait_timed(__atom, old, rel_time); 
#endif //__semaphore_fast_path
        return details::__semaphore_clock::now() < abs_time;
    };
    return details::__binary_semaphore_acquire_slow(__atom, __ticket, __tocket, __stolen, fn);
}

#endif //__semaphore_cuda

#ifndef __semaphore_sem

__semaphore_abi bool counting_semaphore::__fetch_sub_if_slow(counting_semaphore::count_type old)
{
    do
    {
        old &= ~__lockmask;
        if (atom.compare_exchange_weak(old, old - (1 << __shift), std::memory_order_acquire, std::memory_order_relaxed))
            return true;
    } while ((old >> __shift) >= 1);

    return false;
}

#ifdef __semaphore_fast_path
void counting_semaphore::__fetch_add_slow(counting_semaphore::count_type term, counting_semaphore::count_type old, std::memory_order order, semaphore_notify notify)
{
    while (1)
    {
        bool const apply_lock = ((old & __contmask) != 0) && (notify != semaphore_notify::none);
        int const set = ((old & __valumask) + (term << __shift)) | (apply_lock ? __lockmask : 0);

        old &= ~__lockmask;
        if (atom.compare_exchange_weak(old, set, order, std::memory_order_relaxed))
        {
            if (apply_lock)
            {
                switch (notify)
                {
                case semaphore_notify_all:
                    details::__semaphore_wake_all(atom);
                    break;
                case semaphore_notify_one:
                    details::__semaphore_wake_one(atom);
                    break;
                case semaphore_notify_none:
                    break;
                }
                atom.fetch_and(~__lockmask, std::memory_order_relaxed);
            }
            break;
        }
    }
}
#endif //__semaphore_fast_path

__semaphore_abi bool counting_semaphore::__acquire_slow_timed(std::chrono::nanoseconds const&) 
{
    assert(0);
}

__semaphore_abi void counting_semaphore::__acquire_slow()
{

    int old;
    details::__semaphore_exponential_backoff b;
#ifdef __semaphore_fast_path
    for (int i = 0; i < 2; ++i)
    {
#else //__semaphore_fast_path
    while (1)
    {
#endif //__semaphore_fast_path
        b.sleep();
        old = atom.load(std::memory_order_acquire);
        if ((old >> __shift) >= 1)
            goto done;
    }
#ifdef __semaphore_fast_path
    while (1)
    {
        old = atom.fetch_or(__contmask, std::memory_order_relaxed) | __contmask;
        if ((old >> __shift) >= 1)
            goto done;
        details::__semaphore_wait(atom, old);
        old = atom.load(std::memory_order_acquire);
        if ((old >> __shift) >= 1)
            goto done;
    }
#endif //__semaphore_fast_path
done:
#ifdef __semaphore_fast_path
    while (old & __lockmask)
        old = atom.load(std::memory_order_relaxed);
#else //__semaphore_fast_path
    ;
#endif //__semaphore_fast_path
}
#endif //__semaphore_sem

#ifndef __semaphore_cuda

static constexpr int __atomic_wait_table_entry_count = 1024;

__semaphore_managed condition_variable_atomic __atomic_wait_table[__atomic_wait_table_entry_count];

__semaphore_abi size_t __atomic_wait_table_index(void const* ptr) 
{
    return ((uintptr_t)ptr >> 6) & (__atomic_wait_table_entry_count - 1);
}

__semaphore_abi size_t __atomic_wait_table_index(void const volatile* ptr) 
{
    return ((uintptr_t)ptr >> 6) & (__atomic_wait_table_entry_count - 1);
}

__semaphore_abi condition_variable_atomic *condition_variable_atomic::__from_ptr(void const *a)
{
    return __atomic_wait_table + __atomic_wait_table_index(a);
}

__semaphore_abi condition_variable_atomic *condition_variable_atomic::__from_ptr(void const volatile *a)
{
    return __atomic_wait_table + __atomic_wait_table_index(a);
}

#endif
} // v1
} // experimental
} // __semaphore_ns
