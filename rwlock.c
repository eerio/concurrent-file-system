#include <stdlib.h>
#include <pthread.h>
#include <assert.h>

#include "rwlock.h"
#include "err.h"

// Rozwiazanie z labow (przyklady09, readers-writers-template.c)
// Czyli zaadoptowanie rozwiązanie z wykładu/ćwiczeń - nie zagładzamy
// ani czytelników ani pisarzy, poprzez sprawdzanie czy czeka jakiś pisarz
struct rwlock_t {
  pthread_mutex_t mutex;
  pthread_cond_t can_read;
  pthread_cond_t can_write;
  int rcount, wcount, rwait, wwait;
  int change;
};

rwlock_t *rwlock_new() {
  rwlock_t *rwlock = (rwlock_t *)malloc(sizeof(rwlock_t));
  if (!rwlock) { return NULL; }
  assert(!pthread_mutex_init(&rwlock->mutex, NULL));
  assert(!pthread_cond_init(&rwlock->can_read, NULL));
  assert(!pthread_cond_init(&rwlock->can_write, NULL));
  rwlock->rcount = rwlock->wcount = rwlock->rwait = rwlock->wwait = 0;
  rwlock->change = 0;

  return rwlock;
}

void rwlock_destroy(rwlock_t *rwlock) {
  assert(!pthread_mutex_destroy(&rwlock->mutex));
  assert(!pthread_cond_destroy(&rwlock->can_read));
  assert(!pthread_cond_destroy(&rwlock->can_write));
  free(rwlock);
}

void rwlock_rdlock(rwlock_t *rwlock) {
  assert(!pthread_mutex_lock(&rwlock->mutex));
  if (rwlock->wcount + rwlock->wwait > 0 && rwlock->change == 0) {
    do {
      rwlock->rwait++;
      assert(!pthread_cond_wait(&rwlock->can_read, &rwlock->mutex));
      rwlock->rwait--;
    } while (rwlock->wcount > 0 && rwlock->change == 0);
  }
  rwlock->change = 0;
  rwlock->rcount++;

  assert(!pthread_mutex_unlock(&rwlock->mutex));

}

void rwlock_rdunlock(rwlock_t *rwlock) {
  assert(!pthread_mutex_lock(&rwlock->mutex));
  rwlock->rcount--;
  if (rwlock->rcount == 0 && rwlock->wwait > 0) {
    assert(!pthread_cond_signal(&rwlock->can_write));
  }
  assert(!pthread_mutex_unlock(&rwlock->mutex));
}

void rwlock_wrlock(rwlock_t *rwlock) {
  assert(!pthread_mutex_lock(&rwlock->mutex));
  while (rwlock->rcount + rwlock->wcount > 0 || rwlock->change == 1) {
    rwlock->wwait++;
    assert(!pthread_cond_wait(&rwlock->can_write, &rwlock->mutex));
    rwlock->wwait--;
  }
  rwlock->wcount++;
  assert(!pthread_mutex_unlock(&rwlock->mutex));
}

void rwlock_wrunlock(rwlock_t *rwlock) {
  assert(!pthread_mutex_lock(&rwlock->mutex));
  rwlock->wcount--;
  if (rwlock->rwait > 0) {
    rwlock->change = 1;
    assert(!pthread_cond_broadcast(&rwlock->can_read));
  } else if (rwlock->wwait > 0) {
    assert(!pthread_cond_signal(&rwlock->can_write));
  }
  assert(!pthread_mutex_unlock(&rwlock->mutex));
}
