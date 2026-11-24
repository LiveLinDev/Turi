#pragma once

namespace veloz::native {

inline const char* turiConcurrencyRuntimeC() {
    return R"c(
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

typedef void (*tz_conc_run_fn)(void*);
typedef void (*tz_conc_drop_fn)(void*);
typedef void (*tz_conc_pmap_chunk_fn)(void*, int64_t, int64_t);

#define TZ_MAX_WORKERS 16
#define TZ_DEQUE_CAP 1024
#define TZ_STATE_QUEUED 0
#define TZ_STATE_RUNNING 1
#define TZ_STATE_DONE 2

typedef struct tz_conc_task {
    tz_conc_run_fn run;
    tz_conc_drop_fn drop_res;
    struct tz_conc_task* next_in_scope;
    struct tz_conc_task* ov_next;
    int64_t refs;
    int32_t state;
    int32_t consumed;
    void* payload;
} tz_conc_task;

typedef struct tz_conc_scope {
    tz_conc_task* head;
} tz_conc_scope;

#ifdef _WIN32
typedef SRWLOCK tz_lock_t;
typedef CONDITION_VARIABLE tz_cond_t;
static void tz_lock_init(tz_lock_t* l) { InitializeSRWLock(l); }
static void tz_lock_destroy(tz_lock_t* l) { (void)l; }
static void tz_lock_acquire(tz_lock_t* l) { AcquireSRWLockExclusive(l); }
static void tz_lock_release(tz_lock_t* l) { ReleaseSRWLockExclusive(l); }
static int tz_lock_try(tz_lock_t* l) { return TryAcquireSRWLockExclusive(l) != 0; }
static void tz_cond_init(tz_cond_t* c) { InitializeConditionVariable(c); }
static void tz_cond_destroy(tz_cond_t* c) { (void)c; }
static void tz_cond_wait(tz_cond_t* c, tz_lock_t* l) { SleepConditionVariableSRW(c, l, INFINITE, 0); }
static void tz_cond_signal(tz_cond_t* c) { WakeConditionVariable(c); }
static void tz_cond_broadcast(tz_cond_t* c) { WakeAllConditionVariable(c); }
#else
typedef pthread_mutex_t tz_lock_t;
typedef pthread_cond_t tz_cond_t;
static void tz_lock_init(tz_lock_t* l) { pthread_mutex_init(l, NULL); }
static void tz_lock_destroy(tz_lock_t* l) { pthread_mutex_destroy(l); }
static void tz_lock_acquire(tz_lock_t* l) { pthread_mutex_lock(l); }
static void tz_lock_release(tz_lock_t* l) { pthread_mutex_unlock(l); }
static int tz_lock_try(tz_lock_t* l) { return pthread_mutex_trylock(l) == 0; }
static void tz_cond_init(tz_cond_t* c) { pthread_cond_init(c, NULL); }
static void tz_cond_destroy(tz_cond_t* c) { pthread_cond_destroy(c); }
static void tz_cond_wait(tz_cond_t* c, tz_lock_t* l) { pthread_cond_wait(c, l); }
static void tz_cond_signal(tz_cond_t* c) { pthread_cond_signal(c); }
static void tz_cond_broadcast(tz_cond_t* c) { pthread_cond_broadcast(c); }
#endif

typedef struct tz_worker_deque {
    tz_lock_t lock;
    tz_conc_task* ring[TZ_DEQUE_CAP];
    size_t head;
    size_t tail;
    size_t count;
    tz_conc_task* ov_head;
    tz_conc_task* ov_tail;
} tz_worker_deque;

static tz_worker_deque g_deques[TZ_MAX_WORKERS];
#ifdef _WIN32
static HANDLE g_threads[TZ_MAX_WORKERS];
static INIT_ONCE g_pool_once = INIT_ONCE_STATIC_INIT;
#else
static pthread_t g_threads[TZ_MAX_WORKERS];
static pthread_once_t g_pool_once = PTHREAD_ONCE_INIT;
#endif
static tz_lock_t g_wait_lock;
static tz_cond_t g_wait_cv;
static int g_nworkers = 0;
static int32_t g_stop = 0;
static int64_t g_pending = 0;
static uint32_t g_rr_cursor = 0;
static _Thread_local int tls_worker_id = -1;

static void tz_deque_push_bottom(tz_worker_deque* dq, tz_conc_task* t) {
    tz_lock_acquire(&dq->lock);
    if (dq->count < TZ_DEQUE_CAP) {
        dq->ring[dq->tail] = t;
        dq->tail = (dq->tail + 1u) & (TZ_DEQUE_CAP - 1u);
        ++dq->count;
    } else {
        t->ov_next = NULL;
        if (dq->ov_tail) dq->ov_tail->ov_next = t;
        else dq->ov_head = t;
        dq->ov_tail = t;
    }
    tz_lock_release(&dq->lock);
}

static tz_conc_task* tz_deque_pop_bottom(tz_worker_deque* dq) {
    tz_conc_task* t = NULL;
    tz_lock_acquire(&dq->lock);
    if (dq->ov_head) {
        t = dq->ov_head;
        dq->ov_head = t->ov_next;
        if (!dq->ov_head) dq->ov_tail = NULL;
        t->ov_next = NULL;
    } else if (dq->count > 0u) {
        dq->tail = (dq->tail + TZ_DEQUE_CAP - 1u) & (TZ_DEQUE_CAP - 1u);
        t = dq->ring[dq->tail];
        --dq->count;
    }
    tz_lock_release(&dq->lock);
    return t;
}

static tz_conc_task* tz_deque_steal_top(tz_worker_deque* dq, int try_only) {
    tz_conc_task* t = NULL;
    if (try_only) {
        if (!tz_lock_try(&dq->lock)) return NULL;
    } else {
        tz_lock_acquire(&dq->lock);
    }
    if (dq->count > 0u) {
        t = dq->ring[dq->head];
        dq->head = (dq->head + 1u) & (TZ_DEQUE_CAP - 1u);
        --dq->count;
    } else if (dq->ov_head) {
        t = dq->ov_head;
        dq->ov_head = t->ov_next;
        if (!dq->ov_head) dq->ov_tail = NULL;
        t->ov_next = NULL;
    }
    tz_lock_release(&dq->lock);
    return t;
}

static tz_conc_task* tz_pop_any(int self_id) {
    if (self_id >= 0 && self_id < g_nworkers) {
        tz_conc_task* local = tz_deque_pop_bottom(&g_deques[self_id]);
        if (local) {
            __atomic_fetch_sub(&g_pending, 1, __ATOMIC_ACQ_REL);
            return local;
        }
    }
    int n = g_nworkers;
    if (n <= 0) return NULL;
    int start = self_id >= 0 ? self_id : (int)(__atomic_fetch_add(&g_rr_cursor, 1u, __ATOMIC_RELAXED) % (uint32_t)n);
    for (int pass = 0; pass < 2; ++pass) {
        for (int off = 0; off < n; ++off) {
            int idx = (start + off) % n;
            if (idx == self_id) continue;
            tz_conc_task* stolen = tz_deque_steal_top(&g_deques[idx], pass == 0);
            if (stolen) {
                __atomic_fetch_sub(&g_pending, 1, __ATOMIC_ACQ_REL);
                return stolen;
            }
        }
    }
    return NULL;
}

static void tz_exec_task(tz_conc_task* t) {
    int32_t expected = TZ_STATE_QUEUED;
    if (__atomic_compare_exchange_n(&t->state, &expected, TZ_STATE_RUNNING, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        if (t->run) t->run(t->payload);
        tz_lock_acquire(&g_wait_lock);
        __atomic_store_n(&t->state, TZ_STATE_DONE, __ATOMIC_RELEASE);
        tz_cond_broadcast(&g_wait_cv);
        tz_lock_release(&g_wait_lock);
    }
}

static void tz_conc_shutdown(void) {
    if (g_nworkers <= 0) return;
    tz_lock_acquire(&g_wait_lock);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    tz_cond_broadcast(&g_wait_cv);
    tz_lock_release(&g_wait_lock);
    for (int i = 0; i < g_nworkers; ++i) {
#ifdef _WIN32
        if (g_threads[i]) {
            WaitForSingleObject(g_threads[i], INFINITE);
            CloseHandle(g_threads[i]);
            g_threads[i] = NULL;
        }
#else
        pthread_join(g_threads[i], NULL);
#endif
    }
    for (int i = 0; i < g_nworkers; ++i) tz_lock_destroy(&g_deques[i].lock);
    tz_cond_destroy(&g_wait_cv);
    tz_lock_destroy(&g_wait_lock);
    g_nworkers = 0;
}

#ifdef _WIN32
static DWORD WINAPI tz_worker_entry(LPVOID arg) {
    int id = (int)(intptr_t)arg;
    tls_worker_id = id;
    for (;;) {
        tz_conc_task* t = tz_pop_any(id);
        if (t) {
            tz_exec_task(t);
            continue;
        }
        tz_lock_acquire(&g_wait_lock);
        while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) &&
               __atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) == 0) {
            tz_cond_wait(&g_wait_cv, &g_wait_lock);
        }
        int should_stop = __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE);
        tz_lock_release(&g_wait_lock);
        if (should_stop && __atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) == 0) break;
    }
    return 0;
}
#else
static void* tz_worker_entry(void* arg) {
    int id = (int)(intptr_t)arg;
    tls_worker_id = id;
    for (;;) {
        tz_conc_task* t = tz_pop_any(id);
        if (t) {
            tz_exec_task(t);
            continue;
        }
        tz_lock_acquire(&g_wait_lock);
        while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) &&
               __atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) == 0) {
            tz_cond_wait(&g_wait_cv, &g_wait_lock);
        }
        int should_stop = __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE);
        tz_lock_release(&g_wait_lock);
        if (should_stop && __atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) == 0) break;
    }
    return NULL;
}
#endif

static void tz_pool_init_impl(void) {
    tz_lock_init(&g_wait_lock);
    tz_cond_init(&g_wait_cv);
    int n = 4;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    n = (int)si.dwNumberOfProcessors;
#else
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus > 0) n = (int)cpus;
#endif
    if (n < 2) n = 2;
    if (n > TZ_MAX_WORKERS) n = TZ_MAX_WORKERS;
    g_nworkers = n;
    for (int i = 0; i < n; ++i) {
        tz_lock_init(&g_deques[i].lock);
        g_deques[i].head = 0;
        g_deques[i].tail = 0;
        g_deques[i].count = 0;
        g_deques[i].ov_head = NULL;
        g_deques[i].ov_tail = NULL;
    }
    for (int i = 0; i < n; ++i) {
#ifdef _WIN32
        g_threads[i] = CreateThread(NULL, 0, tz_worker_entry, (LPVOID)(intptr_t)i, 0, NULL);
#else
        pthread_create(&g_threads[i], NULL, tz_worker_entry, (void*)(intptr_t)i);
#endif
    }
    atexit(tz_conc_shutdown);
}

#ifdef _WIN32
static BOOL CALLBACK tz_pool_init_cb(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    tz_pool_init_impl();
    return TRUE;
}
static void tz_pool_ensure(void) {
    InitOnceExecuteOnce(&g_pool_once, tz_pool_init_cb, NULL, NULL);
}
#else
static void tz_pool_ensure(void) {
    pthread_once(&g_pool_once, tz_pool_init_impl);
}
#endif

static void tz_task_release(tz_conc_task* t) {
    if (!t) return;
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        if (!__atomic_load_n(&t->consumed, __ATOMIC_ACQUIRE) && t->drop_res && t->payload) {
            t->drop_res(t->payload);
        }
        free(t);
    }
}

void* tz_conc_scope_new(void) {
    tz_pool_ensure();
    return calloc(1, sizeof(tz_conc_scope));
}

void* tz_conc_task_alloc(void* scope_ptr, int64_t payload_bytes, tz_conc_run_fn run, tz_conc_drop_fn drop_res) {
    tz_conc_scope* sc = (tz_conc_scope*)scope_ptr;
    if (!sc) return NULL;
    tz_pool_ensure();
    size_t hdr = (sizeof(tz_conc_task) + 15u) & ~(size_t)15u;
    size_t pbytes = payload_bytes > 0 ? (size_t)payload_bytes : 16u;
    tz_conc_task* t = (tz_conc_task*)calloc(1, hdr + pbytes);
    if (!t) return NULL;
    t->run = run;
    t->drop_res = drop_res;
    t->refs = 2;
    t->state = TZ_STATE_QUEUED;
    t->consumed = 0;
    t->payload = (char*)t + hdr;
    t->next_in_scope = sc->head;
    sc->head = t;
    return (void*)t;
}

void* tz_conc_task_payload(void* task_ptr) {
    tz_conc_task* t = (tz_conc_task*)task_ptr;
    return t ? t->payload : NULL;
}

void tz_conc_task_submit(void* task_ptr) {
    tz_conc_task* t = (tz_conc_task*)task_ptr;
    if (!t) return;
    tz_pool_ensure();
    int wid = tls_worker_id;
    int target = (wid >= 0 && wid < g_nworkers)
        ? wid
        : (int)(__atomic_fetch_add(&g_rr_cursor, 1u, __ATOMIC_RELAXED) % (uint32_t)g_nworkers);
    __atomic_fetch_add(&g_pending, 1, __ATOMIC_RELEASE);
    tz_deque_push_bottom(&g_deques[target], t);
    tz_lock_acquire(&g_wait_lock);
    tz_cond_signal(&g_wait_cv);
    tz_lock_release(&g_wait_lock);
}

void tz_conc_task_wait(void* task_ptr) {
    tz_conc_task* t = (tz_conc_task*)task_ptr;
    if (!t) return;
    int wid = tls_worker_id;
    for (;;) {
        if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == TZ_STATE_DONE) break;
        tz_conc_task* other = tz_pop_any(wid);
        if (other) {
            tz_exec_task(other);
            continue;
        }
        tz_lock_acquire(&g_wait_lock);
        if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != TZ_STATE_DONE &&
            __atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) == 0) {
            tz_cond_wait(&g_wait_cv, &g_wait_lock);
        }
        tz_lock_release(&g_wait_lock);
    }
}

void tz_conc_task_join_done(void* task_ptr) {
    tz_conc_task* t = (tz_conc_task*)task_ptr;
    if (!t) return;
    tz_conc_task_wait(t);
    __atomic_store_n(&t->consumed, 1, __ATOMIC_RELEASE);
    tz_task_release(t);
}

void tz_conc_task_drop(void** task_slot) {
    if (!task_slot || !*task_slot) return;
    tz_conc_task* t = (tz_conc_task*)*task_slot;
    *task_slot = NULL;
    tz_conc_task_wait(t);
    tz_task_release(t);
}

void tz_conc_scope_drop(void** scope_slot) {
    if (!scope_slot || !*scope_slot) return;
    tz_conc_scope* sc = (tz_conc_scope*)*scope_slot;
    *scope_slot = NULL;
    tz_conc_task* cur = sc->head;
    sc->head = NULL;
    while (cur) {
        tz_conc_task* next = cur->next_in_scope;
        tz_conc_task_wait(cur);
        tz_task_release(cur);
        cur = next;
    }
    free(sc);
}

typedef struct tz_pmap_chunk {
    void* user_ctx;
    tz_conc_pmap_chunk_fn fn;
    int64_t lo;
    int64_t hi;
} tz_pmap_chunk;

static void tz_pmap_chunk_run(void* p) {
    tz_pmap_chunk* ch = (tz_pmap_chunk*)p;
    ch->fn(ch->user_ctx, ch->lo, ch->hi);
}

void tz_conc_parallel_for(int64_t n, void* user_ctx, tz_conc_pmap_chunk_fn fn) {
    if (n <= 0 || !fn) return;
    if (n == 1) {
        fn(user_ctx, 0, 1);
        return;
    }
    tz_pool_ensure();
    int max_chunks = g_nworkers * 4;
    if (max_chunks > 64) max_chunks = 64;
    if (n < (int64_t)max_chunks) max_chunks = (int)n;

    tz_conc_task tasks[64];
    tz_pmap_chunk chunks[64];
    int64_t base = n / max_chunks;
    int64_t rem = n % max_chunks;
    int64_t cur = 0;
    for (int k = 0; k < max_chunks; ++k) {
        int64_t sz = base + (k < rem ? 1 : 0);
        chunks[k].user_ctx = user_ctx;
        chunks[k].fn = fn;
        chunks[k].lo = cur;
        chunks[k].hi = cur + sz;
        cur += sz;
        tasks[k].run = tz_pmap_chunk_run;
        tasks[k].drop_res = NULL;
        tasks[k].next_in_scope = NULL;
        tasks[k].ov_next = NULL;
        tasks[k].refs = 1;
        tasks[k].state = (k == 0) ? TZ_STATE_RUNNING : TZ_STATE_QUEUED;
        tasks[k].consumed = 1;
        tasks[k].payload = &chunks[k];
    }
    for (int k = max_chunks - 1; k >= 1; --k) {
        tz_conc_task_submit(&tasks[k]);
    }
    fn(user_ctx, chunks[0].lo, chunks[0].hi);
    for (int k = 1; k < max_chunks; ++k) {
        tz_conc_task_wait(&tasks[k]);
    }
}

typedef struct tz_conc_ring {
    int64_t rc;
    size_t mask;
    size_t elem_size;
    size_t slot_stride;
    tz_conc_drop_fn drop_elem;
    uint8_t* slots;
    char pad0[64];
    size_t enqueue_pos;
    char pad1[64 - sizeof(size_t)];
    size_t dequeue_pos;
    char pad2[64 - sizeof(size_t)];
} tz_conc_ring;

void* tz_conc_ring_new(int64_t cap, int64_t elem_size, tz_conc_drop_fn drop_elem) {
    if (cap <= 0 || cap > (1LL << 24)) return NULL;
    size_t real_cap = 2u;
    while (real_cap < (size_t)cap) real_cap <<= 1u;
    size_t esz = elem_size > 0 ? (size_t)elem_size : 0u;
    size_t stride = (sizeof(size_t) + esz + sizeof(size_t) - 1u) & ~(sizeof(size_t) - 1u);
    if (real_cap > SIZE_MAX / stride) return NULL;
    tz_conc_ring* q = (tz_conc_ring*)calloc(1, sizeof(tz_conc_ring));
    if (!q) return NULL;
    uint8_t* slots = (uint8_t*)calloc(real_cap, stride);
    if (!slots) {
        free(q);
        return NULL;
    }
    for (size_t i = 0; i < real_cap; ++i) {
        *(size_t*)(slots + i * stride) = i;
    }
    q->rc = 1;
    q->mask = real_cap - 1u;
    q->elem_size = esz;
    q->slot_stride = stride;
    q->drop_elem = drop_elem;
    q->slots = slots;
    q->enqueue_pos = 0u;
    q->dequeue_pos = 0u;
    return (void*)q;
}

void* tz_conc_ring_retain(void* qp) {
    tz_conc_ring* q = (tz_conc_ring*)qp;
    if (q) __atomic_fetch_add(&q->rc, 1, __ATOMIC_RELAXED);
    return qp;
}

void tz_conc_ring_release(void** slot) {
    if (!slot || !*slot) return;
    tz_conc_ring* q = (tz_conc_ring*)*slot;
    *slot = NULL;
    if (__atomic_sub_fetch(&q->rc, 1, __ATOMIC_ACQ_REL) == 0) {
        if (q->drop_elem && q->slots) {
            size_t cap = q->mask + 1u;
            size_t head = q->dequeue_pos;
            size_t tail = q->enqueue_pos;
            for (size_t pos = head; pos != tail && (pos - head) < cap; ++pos) {
                uint8_t* cell = q->slots + (pos & q->mask) * q->slot_stride;
                size_t seq = *(size_t*)cell;
                if (seq == pos + 1u) {
                    q->drop_elem((void*)(cell + sizeof(size_t)));
                }
            }
        }
        free(q->slots);
        free(q);
    }
}

uint8_t tz_conc_ring_try_push(void* qp, const void* src) {
    tz_conc_ring* q = (tz_conc_ring*)qp;
    if (!q) return 0u;
    size_t pos = __atomic_load_n(&q->enqueue_pos, __ATOMIC_RELAXED);
    for (;;) {
        uint8_t* cell = q->slots + (pos & q->mask) * q->slot_stride;
        size_t* seqp = (size_t*)cell;
        size_t seq = __atomic_load_n(seqp, __ATOMIC_ACQUIRE);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;
        if (diff == 0) {
            if (__atomic_compare_exchange_n(&q->enqueue_pos, &pos, pos + 1u, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                if (q->elem_size > 0u && src) memcpy(cell + sizeof(size_t), src, q->elem_size);
                __atomic_store_n(seqp, pos + 1u, __ATOMIC_RELEASE);
                return 1u;
            }
        } else if (diff < 0) {
            return 0u;
        } else {
            pos = __atomic_load_n(&q->enqueue_pos, __ATOMIC_RELAXED);
        }
    }
}

uint8_t tz_conc_ring_try_pop(void* qp, void* dst) {
    tz_conc_ring* q = (tz_conc_ring*)qp;
    if (!q) return 0u;
    size_t pos = __atomic_load_n(&q->dequeue_pos, __ATOMIC_RELAXED);
    for (;;) {
        uint8_t* cell = q->slots + (pos & q->mask) * q->slot_stride;
        size_t* seqp = (size_t*)cell;
        size_t seq = __atomic_load_n(seqp, __ATOMIC_ACQUIRE);
        intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1u);
        if (diff == 0) {
            if (__atomic_compare_exchange_n(&q->dequeue_pos, &pos, pos + 1u, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                if (q->elem_size > 0u && dst) memcpy(dst, cell + sizeof(size_t), q->elem_size);
                __atomic_store_n(seqp, pos + q->mask + 1u, __ATOMIC_RELEASE);
                return 1u;
            }
        } else if (diff < 0) {
            return 0u;
        } else {
            pos = __atomic_load_n(&q->dequeue_pos, __ATOMIC_RELAXED);
        }
    }
}

int64_t tz_conc_ring_len(void* qp) {
    tz_conc_ring* q = (tz_conc_ring*)qp;
    if (!q) return 0;
    size_t tail = __atomic_load_n(&q->enqueue_pos, __ATOMIC_ACQUIRE);
    size_t head = __atomic_load_n(&q->dequeue_pos, __ATOMIC_ACQUIRE);
    size_t cap = q->mask + 1u;
    intptr_t d = (intptr_t)(tail - head);
    if (d < 0) return 0;
    if ((size_t)d > cap) return (int64_t)cap;
    return (int64_t)d;
}

int64_t tz_conc_ring_cap(void* qp) {
    tz_conc_ring* q = (tz_conc_ring*)qp;
    return q ? (int64_t)(q->mask + 1u) : 0;
}
)c";
}

} // namespace veloz::native
