/*************************************************************************Copyright (c) 2010-2015 Helmholtz-Zentrum Berlin f. Materialien
                        und Energie GmbH, Germany (HZB)
This file is distributed subject to a Software License Agreement found
in the file LICENSE that is included with this distribution.
\*************************************************************************/
#include "seq.h"
#include "seq_debug.h"
#include "epicsAtomic.h"

struct seqQueue {
    size_t          wr;
    size_t          rd;
    size_t          numElems;
    size_t          elemSize;
    boolean         overflow;
    epicsMutexId    mutex;
    char            *buffer;
};

epicsShareFunc boolean seqQueueInvariant(QUEUE q)
{
    return (q != NULL)
        && q->elemSize > 0
        && q->numElems > 0
        && q->numElems <= seqQueueMaxNumElems
        && epicsAtomicGetSizeT(&q->rd) < q->numElems
        && epicsAtomicGetSizeT(&q->wr) < q->numElems;
}

epicsShareFunc QUEUE seqQueueCreate(size_t numElems, size_t elemSize)
{
    QUEUE q = new(struct seqQueue);

    if (!q) {
        errlogSevPrintf(errlogFatal, "seqQueueCreate: out of memory\n");
        return 0;
    }
    /* check arguments to establish invariants */
    if (numElems == 0) {
        errlogSevPrintf(errlogFatal, "seqQueueCreate: numElems must be positive\n");
        free(q);
        return 0;
    }
    if (elemSize == 0) {
        errlogSevPrintf(errlogFatal, "seqQueueCreate: elemSize must be positive\n");
        free(q);
        return 0;
    }
    if (numElems > seqQueueMaxNumElems) {
        errlogSevPrintf(errlogFatal, "seqQueueCreate: numElems too large\n");
        free(q);
        return 0;
    }
    DEBUG("%s:%d:calloc(%u,%u)\n",__FILE__,__LINE__,numElems, elemSize);
    q->buffer = (char *)calloc(numElems, elemSize);
    if (!q->buffer) {
        errlogSevPrintf(errlogFatal, "seqQueueCreate: out of memory\n");
        free(q);
        return 0;
    }
    q->mutex = epicsMutexCreate();
    if (!q->mutex) {
        errlogSevPrintf(errlogFatal, "seqQueueCreate: out of memory\n");
        free(q->buffer);
        free(q);
        return 0;
    }
    q->elemSize = elemSize;
    q->numElems = numElems;
    q->overflow = FALSE;
    q->rd = q->wr = 0;
    return q;
}

epicsShareFunc void seqQueueDestroy(QUEUE q)
{
    epicsMutexDestroy(q->mutex);
    free(q->buffer);
    free(q);
}

epicsShareFunc boolean seqQueueGet(QUEUE q, void *value)
{
    return seqQueueGetF(q, memcpy, value);
}

epicsShareFunc boolean seqQueueGetF(QUEUE q, seqQueueFunc *get, void *arg)
{
    size_t rd = epicsAtomicGetSizeT(&q->rd);
    size_t wr = epicsAtomicGetSizeT(&q->wr);

    if (wr == rd) {
        if (!q->overflow) {
            return TRUE;
        }
        epicsMutexLock(q->mutex);
        rd = epicsAtomicGetSizeT(&q->rd);
        wr = epicsAtomicGetSizeT(&q->wr);
        get(arg, q->buffer + rd * q->elemSize, q->elemSize);
        /* check again, a put might have intervened */
        if (wr == rd && q->overflow) {
            q->overflow = FALSE;
        } else {
            epicsAtomicSetSizeT(&q->rd, (rd + 1) % q->numElems);
        }
        epicsMutexUnlock(q->mutex);
    } else {
        epicsAtomicReadMemoryBarrier();
        get(arg, q->buffer + rd * q->elemSize, q->elemSize);
        epicsAtomicSetSizeT(&q->rd, (rd + 1) % q->numElems);
    }
    return FALSE;
}

epicsShareFunc boolean seqQueuePut(QUEUE q, const void *value)
{
    return seqQueuePutF(q, memcpy, value);
}

epicsShareFunc boolean seqQueuePutF(QUEUE q, seqQueueFunc *put, const void *arg)
{
    boolean r = FALSE;
    size_t rd = epicsAtomicGetSizeT(&q->rd);
    size_t wr = epicsAtomicGetSizeT(&q->wr);

    if (q->overflow || (wr + 1) % q->numElems == rd) {
        epicsMutexLock(q->mutex);
        rd = epicsAtomicGetSizeT(&q->rd);
        wr = epicsAtomicGetSizeT(&q->wr);
        if ((wr + 1) % q->numElems == rd) {
            if (q->overflow) {
                r = TRUE;   /* we will overwrite the last element */
            }
            q->overflow = TRUE;
        } else if (q->overflow) {
            /* we had a get since the last put, so
               can now eliminate overflow flag and instead
               increment the write pointer */
            wr = (wr + 1) % q->numElems;
            epicsAtomicSetSizeT(&q->wr, wr);
            if ((wr + 1) % q->numElems != rd) {
                q->overflow = FALSE;
            }
        }
        put(q->buffer + wr * q->elemSize, arg, q->elemSize);
        if (!q->overflow) {
            epicsAtomicWriteMemoryBarrier();
            epicsAtomicSetSizeT(&q->wr, (wr + 1) % q->numElems);
        }
        epicsMutexUnlock(q->mutex);
    } else {
        put(q->buffer + wr * q->elemSize, arg, q->elemSize);
        epicsAtomicWriteMemoryBarrier();
        epicsAtomicSetSizeT(&q->wr, (wr + 1) % q->numElems);
    }
    return r;
}

epicsShareFunc void seqQueueFlush(QUEUE q)
{
    epicsMutexLock(q->mutex);
    epicsAtomicSetSizeT(&q->rd, epicsAtomicGetSizeT(&q->wr));
    q->overflow = FALSE;
    epicsMutexUnlock(q->mutex);
}

static size_t used(const QUEUE q)
{
    size_t rd = epicsAtomicGetSizeT(&q->rd);
    size_t wr = epicsAtomicGetSizeT(&q->wr);
    return (q->numElems + wr - rd) % q->numElems + (q->overflow ? 1 : 0);
}

epicsShareFunc size_t seqQueueFree(const QUEUE q)
{
    return q->numElems - used(q);
}

epicsShareFunc size_t seqQueueUsed(const QUEUE q)
{
    return used(q);
}

epicsShareFunc boolean seqQueueIsEmpty(const QUEUE q)
{
    size_t rd = epicsAtomicGetSizeT(&q->rd);
    size_t wr = epicsAtomicGetSizeT(&q->wr);
    return wr == rd && !q->overflow;
}

epicsShareFunc boolean seqQueueIsFull(const QUEUE q)
{
    size_t rd = epicsAtomicGetSizeT(&q->rd);
    size_t wr = epicsAtomicGetSizeT(&q->wr);
    return (wr + 1) % q->numElems == rd && q->overflow;
}

epicsShareFunc size_t seqQueueNumElems(const QUEUE q)
{
    return q->numElems;
}

epicsShareFunc size_t seqQueueElemSize(const QUEUE q)
{
    return q->elemSize;
}
