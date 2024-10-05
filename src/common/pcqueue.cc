/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "common/pcqueue.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>

#include "common/massert.h"
#include "devtools/TracePrinter.h"
#include "errors/sfserr.h"
#include "slogger/slogger.h"

typedef struct _qentry {
	uint32_t id;
	uint32_t op;
	uint8_t *data;
	uint32_t leng;
	struct _qentry *next;
} qentry;

typedef struct _queue {
	qentry *head,**tail;
	uint32_t elements;
	uint32_t size;
	uint32_t maxsize;
	uint32_t freewaiting;
	uint32_t fullwaiting;
	pthread_cond_t waitfree,waitfull;
	pthread_mutex_t lock;
} queue;

void* queue_new(uint32_t size) {
	TRACETHIS();
	queue *q;
	q = (queue*)malloc(sizeof(queue));
	passert(q);
	q->head = NULL;
	q->tail = &(q->head);
	q->elements = 0;
	q->size = 0;
	q->maxsize = size;
	q->freewaiting = 0;
	q->fullwaiting = 0;
	if (size) {
		zassert(pthread_cond_init(&(q->waitfull),NULL));
	}
	zassert(pthread_cond_init(&(q->waitfree),NULL));
	zassert(pthread_mutex_init(&(q->lock),NULL));
	return q;
}

/// Waits for all threads to finish.
/// Must be called with the queue lock held.
void queue_wait_for_threads_to_finish(queue *que) {
	TRACETHIS();

	// Wait up to 20 seconds for all threads to finish their jobs
	constexpr uint32_t kMaxRetries = 100;
	constexpr uint32_t kWaitTimeMs = 200;

	for (uint32_t retry = 0; que->freewaiting > 0 && retry < kMaxRetries;
	     ++retry) {
		safs_pretty_syslog(LOG_NOTICE,
		                   "pcqueue: Waiting %d ms for %u threads to "
		                   "finish (retry %d of %d)",
		                   kWaitTimeMs, que->freewaiting, retry, kMaxRetries);

		zassert(pthread_cond_signal(&(que->waitfree)));
		que->freewaiting--;

		zassert(pthread_mutex_unlock(&(que->lock)));
		std::this_thread::sleep_for(std::chrono::milliseconds(kWaitTimeMs));
		zassert(pthread_mutex_lock(&(que->lock)));
	}
}

void queue_delete(void *que, void (*deleter)(uint8_t *)) {
	TRACETHIS();
	queue *q = (queue*)que;
	qentry *qe,*qen;
	zassert(pthread_mutex_lock(&(q->lock)));

	if (q->freewaiting > 0) {
		queue_wait_for_threads_to_finish(q);
	}

	sassert(q->freewaiting==0);
	sassert(q->fullwaiting==0);
	for (qe = q->head ; qe ; qe = qen) {
		qen = qe->next;
		deleter(qe->data);
		free(qe);
	}
	zassert(pthread_mutex_unlock(&(q->lock)));
	zassert(pthread_mutex_destroy(&(q->lock)));
	zassert(pthread_cond_destroy(&(q->waitfree)));
	if (q->maxsize) {
		zassert(pthread_cond_destroy(&(q->waitfull)));
	}
	free(q);
}

int queue_isempty(void *que) {
	TRACETHIS();
	queue *q = (queue*)que;
	int r;
	zassert(pthread_mutex_lock(&(q->lock)));
	r=(q->elements==0)?1:0;
	zassert(pthread_mutex_unlock(&(q->lock)));
	return r;
}

uint32_t queue_elements(void *que) {
	TRACETHIS();
	queue *q = (queue*)que;
	uint32_t r;
	zassert(pthread_mutex_lock(&(q->lock)));
	r=q->elements;
	zassert(pthread_mutex_unlock(&(q->lock)));
	return r;
}

int queue_isfull(void *que) {
	TRACETHIS();
	queue *q = (queue*)que;
	int r;
	zassert(pthread_mutex_lock(&(q->lock)));
	r = (q->maxsize>0 && q->maxsize<=q->size)?1:0;
	zassert(pthread_mutex_unlock(&(q->lock)));
	return r;
}

uint32_t queue_sizeleft(void *que) {
	TRACETHIS();
	queue *q = (queue*)que;
	uint32_t r;
	zassert(pthread_mutex_lock(&(q->lock)));
	if (q->maxsize>0) {
		r = q->maxsize-q->size;
	} else {
		r = 0xFFFFFFFF;
	}
	zassert(pthread_mutex_unlock(&(q->lock)));
	return r;
}

int queue_put(void *que,uint32_t id,uint32_t op,uint8_t *data,uint32_t leng) {
	TRACETHIS();
	queue *q = (queue*)que;
	qentry *qe;
	qe = (qentry*) malloc(sizeof(qentry));
	passert(qe);
	qe->id = id;
	qe->op = op;
	qe->data = data;
	qe->leng = leng;
	qe->next = NULL;
	zassert(pthread_mutex_lock(&(q->lock)));
	if (q->maxsize) {
		if (leng>q->maxsize) {
			zassert(pthread_mutex_unlock(&(q->lock)));
			free(qe);
			errno = EDEADLK;
			return -1;
		}
		while (q->size+leng>q->maxsize) {
			q->fullwaiting++;
			zassert(pthread_cond_wait(&(q->waitfull),&(q->lock)));
		}
	}
	q->elements++;
	q->size += leng;
	*(q->tail) = qe;
	q->tail = &(qe->next);
	if (q->freewaiting>0) {
		zassert(pthread_cond_signal(&(q->waitfree)));
		q->freewaiting--;
	}
	zassert(pthread_mutex_unlock(&(q->lock)));
	return 0;
}

int queue_tryput(void *que,uint32_t id,uint32_t op,uint8_t *data,uint32_t leng) {
	TRACETHIS();
	queue *q = (queue*)que;
	qentry *qe;
	zassert(pthread_mutex_lock(&(q->lock)));
	if (q->maxsize) {
		if (leng>q->maxsize) {
			zassert(pthread_mutex_unlock(&(q->lock)));
			errno = EDEADLK;
			return -1;
		}
		if (q->size+leng>q->maxsize) {
			zassert(pthread_mutex_unlock(&(q->lock)));
			errno = EBUSY;
			return -1;
		}
	}
	qe = (qentry*) malloc(sizeof(qentry));
	passert(qe);
	qe->id = id;
	qe->op = op;
	qe->data = data;
	qe->leng = leng;
	qe->next = NULL;
	q->elements++;
	q->size += leng;
	*(q->tail) = qe;
	q->tail = &(qe->next);
	if (q->freewaiting>0) {
		zassert(pthread_cond_signal(&(q->waitfree)));
		q->freewaiting--;
	}
	zassert(pthread_mutex_unlock(&(q->lock)));
	return 0;
}

int queue_get(void *que,uint32_t *id,uint32_t *op,uint8_t **data,uint32_t *leng) {
	TRACETHIS();
	queue *q = (queue*)que;
	qentry *qe;
	zassert(pthread_mutex_lock(&(q->lock)));
	while (q->elements==0) {
		q->freewaiting++;
		zassert(pthread_cond_wait(&(q->waitfree),&(q->lock)));
	}
	qe = q->head;
	q->head = qe->next;
	if (q->head==NULL) {
		q->tail = &(q->head);
	}
	q->elements--;
	q->size -= qe->leng;
	if (q->fullwaiting>0) {
		zassert(pthread_cond_signal(&(q->waitfull)));
		q->fullwaiting--;
	}
	zassert(pthread_mutex_unlock(&(q->lock)));
	if (id) {
		*id = qe->id;
	}
	if (op) {
		*op = qe->op;
	}
	if (data) {
		*data = qe->data;
	}
	if (leng) {
		*leng = qe->leng;
	}
	free(qe);
	return 0;
}

int queue_tryget(void *que,uint32_t *id,uint32_t *op,uint8_t **data,uint32_t *leng) {
	TRACETHIS();
	queue *q = (queue*)que;
	qentry *qe;
	zassert(pthread_mutex_lock(&(q->lock)));
	if (q->elements==0) {
		zassert(pthread_mutex_unlock(&(q->lock)));
		if (id) {
			*id=0;
		}
		if (op) {
			*op=0;
		}
		if (data) {
			*data=NULL;
		}
		if (leng) {
			*leng=0;
		}
		errno = EBUSY;
		return -1;
	}
	qe = q->head;
	q->head = qe->next;
	if (q->head==NULL) {
		q->tail = &(q->head);
	}
	q->elements--;
	q->size -= qe->leng;
	if (q->fullwaiting>0) {
		zassert(pthread_cond_signal(&(q->waitfull)));
		q->fullwaiting--;
	}
	zassert(pthread_mutex_unlock(&(q->lock)));
	if (id) {
		*id = qe->id;
	}
	if (op) {
		*op = qe->op;
	}
	if (data) {
		*data = qe->data;
	}
	if (leng) {
		*leng = qe->leng;
	}
	free(qe);
	return 0;
}
