#pragma once

typedef struct rwlock_t rwlock_t;

rwlock_t *rwlock_new();
void rwlock_destroy(rwlock_t *rwlock);
void rwlock_rdlock(rwlock_t *rwlock);
void rwlock_rdunlock(rwlock_t *rwlock);
void rwlock_wrlock(rwlock_t *rwlock);
void rwlock_wrunlock(rwlock_t *rwlock);
