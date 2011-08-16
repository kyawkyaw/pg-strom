/*
 * msegment.c - routines to support shared memory segment
 *
 * Copyright (c) 2011 KaiGai Kohei <kaigai@kaigai.gr.jp>
 */
#include "postgres.h"
#include "pg_boost.h"
#include "utils/guc.h"
#include <sys/ipc.h>
#include <sys/shm.h>

#define MSEGMENT_CLASS_MIN_BITS		6	/* 64bytes */
#define MSEGMENT_CLASS_MAX_BITS		31	/* 2GBytes */
#define MSEGMENT_CLASS_MIN_SIZE		(1 << MSEGMENT_CLASS_MIN_BITS)
#define MSEGMENT_CLASS_MAX_SIZE		(1 << MSEGMENT_CLASS_MAX_BITS)
#define MSEGMENT_BUFFER_NUM_SLOTS	64

struct msegment_t {
	int				shmid;
	uintptr_t		segment_size;
	uintptr_t		segment_usage;
	mlist_t			free_list[MSEGMENT_CLASS_MAX_BITS + 1];
	int				num_active[MSEGMENT_CLASS_MAX_BITS + 1];
	int				num_free[MSEGMENT_CLASS_MAX_BITS + 1];
	pthread_mutex_t	lock;
	/*
	 * memory buffer stuff
	 */
	mlist_t			mbuffer_list[MSEGMENT_BUFFER_NUM_SLOTS];
	pthread_mutex_t	mbuffer_lock[MSEGMENT_BUFFER_NUM_SLOTS];
	int				mbuffer_index;
	int				mbuffer_reclaim;
	uintptr_t		mbuffer_size;
	uintptr_t		mbuffer_usage;
};
typedef struct msegment_t	msegment_t;

#define MCHUNK_TAG_FREE		0x01
#define MCHUNK_TAG_ITEM		0x02
#define MCHUNK_TAG_BUFFER	0x03

struct mchunk_free_t {
	uint16		mclass;
	uint8		mtag;
	mlist_t		list;
};
typedef struct mchunk_free_t	mchunk_free_t;

struct mchunk_item_t {
	uint16		mclass;
	uint8		mtag;
	uintptr_t	data[0];	/* data should be aligned */
};
typedef struct mchunk_item_t	mchunk_item_t;

#define MBUFFER_FLAG_DIRTY_CACHE		0x01
#define MBUFFER_FLAG_HOT_CACHE			0x02
#define MBUFFER_FLAG_COMPRESSED			0x04

struct mchunk_buffer_t {
	uint16		mclass;
	uint8		mtag;
	uint8		flags;
	uintptr_t	storage;
	uintptr_t	buffered;
	mlist_t		list;
	uint32		length;
	pthread_rwlock_t	lock;
};
typedef struct mchunk_buffer_t	mchunk_buffer_t;

/*
 * Global/Local Variables
 */
msegment_t *msegment = NULL;

static pthread_mutexattr_t	msegment_mutex_attr;
static pthread_rwlockattr_t	msegment_rwlock_attr;

/*
 * ffs - returns first (smallest) bit of the value
 */
static inline int fast_ffs(uintptr_t value)
{
	return __builtin_ffsl((unsigned long)value);
}

/*
 * fls - returns last (biggest) bit of the value
 */
static inline int fast_fls(uintptr_t value)
{
	if (!value)
		return 0;
	return sizeof(value) * 8 - __builtin_clz(value);
}

static bool
shmseg_split_chunk(int mclass)
{
	mchunk_free_t  *mfree1;
	mchunk_free_t  *mfree2;
	mlist_t		   *mlist;
	uintptr_t		offset;

	Assert(mclass > MSEGMENT_CLASS_MIN_BITS &&
		   mclass <= MSEGMENT_CLASS_MAX_BITS);

	if (mlist_empty(&msegment->free_list[mclass]))
	{
		if (mclass == MSEGMENT_CLASS_MAX_BITS)
			return false;
		else if (!shmseg_split_chunk(mclass + 1))
			return false;
	}
	mlist = offset_to_addr(msegment->free_list[mclass].next);
	mfree1 = container_of(mlist, mchunk_free_t, list);
	Assert(mfree1->mclass == mclass);

	mlist_del(&mfree1->list);
	msegment->num_free[mclass]--;

	offset = addr_to_offset(mfree1);
	mclass--;
	mfree2 = offset_to_addr(offset + (1 << mclass));

	mfree1->mclass = mfree2->mclass = mclass;
	mfree1->mtag = mfree2->mtag = MCHUNK_TAG_FREE;

	mlist_add(&msegment->free_list[mclass], &mfree1->list);
	mlist_add(&msegment->free_list[mclass], &mfree2->list);
	msegment->num_free[mclass] -= 2;

	return true;
}

void *
shmseg_try_alloc(size_t size)
{
	mchunk_item_t  *mitem;
	mlist_t		   *mlist;
	int				mclass;
	bool			retried = false;

	mclass = fast_fls(size + offsetof(mchunk_item_t, data) - 1);
	if (mclass > MSEGMENT_CLASS_MAX_BITS)
		return NULL;
	if (mclass < MSEGMENT_CLASS_MIN_BITS)
		mclass = MSEGMENT_CLASS_MIN_BITS;

retry:
	pthread_mutex_lock(&msegment->lock);

	/*
	 * If no mchunk_free item is on free_list[mclass], we try to split
	 * a larger free chunk, and allocate it.
	 * If unavailable anymore, we cannot allocate a new free chunk.
	 */
	if (mlist_empty(&msegment->free_list[mclass]))
	{
		if (!shmseg_split_chunk(mclass + 1))
		{
			pthread_mutex_unlock(&msegment->lock);
			if (!retried)
			{
				retried = true;
				goto retry;
			}
			return NULL;
		}
	}
	Assert(!mlist_empty(&msegment->free_list[mclass]));

	mlist = offset_to_addr(msegment->free_list[mclass].next);
	mitem = (mchunk_item_t *)container_of(mlist, mchunk_free_t, list);
	mlist_del(mlist);

	Assert(mitem->mclass == mclass);
	mitem->mtag = MCHUNK_TAG_ITEM;

	msegment->num_free[mclass]--;
	msegment->num_active[mclass]++;
	msegment->segment_usage += (1 << mclass);

	pthread_mutex_unlock(&msegment->lock);

	return (void *)mitem->data;
}

void *
shmseg_alloc(size_t size)
{
	void   *result = shmseg_try_alloc(size);

	if (!result)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_boost: out of shared memory")));
	return result;
}

void *
shmseg_try_resize(void *addr, size_t new_size)
{
	mchunk_item_t  *mitem = container_of(addr, mchunk_item_t, data);
	uintptr_t		offset_s;
	uintptr_t		offset_e;
	int				mclass_new;

	Assert(mitem->mtag == MCHUNK_TAG_ITEM);

	mclass_new = fast_fls(new_size + offsetof(mchunk_item_t, data) - 1);
	if (mclass_new > MSEGMENT_CLASS_MAX_BITS)
		return NULL;
	if (mclass_new < MSEGMENT_CLASS_MIN_BITS)
		mclass_new = MSEGMENT_CLASS_MIN_BITS;

	/* no need to change */
	if (mitem->mclass == mclass_new)
		return mitem->data;

	/* expand case */
	if (mitem->mclass < mclass_new)
	{
		void   *data_new = shmseg_try_alloc(new_size);

		if (data_new)
		{
			memcpy(data_new, mitem->data,
				   (1 << mitem->mclass) - offsetof(mchunk_item_t, data));
			shmseg_free(addr);
		}
		return data_new;
	}

	/* reduction case */
	offset_s = addr_to_offset(mitem) + (1 << mclass_new);
	offset_e = addr_to_offset(mitem) + (1 << mitem->mclass);

	pthread_mutex_lock(&msegment->lock);

	while (offset_s < offset_e)
	{
		mchunk_free_t  *mfree = offset_to_addr(offset_s);
		int				mclass = fast_ffs(offset_s);

		Assert(mclass >= MSEGMENT_CLASS_MIN_BITS &&
			   mclass < MSEGMENT_CLASS_MAX_BITS);
		Assert(offset_s + (1 << mclass) <= offset_e);

		/* chain this free chunk to free_list */
		mfree->mclass = mclass;
		mfree->mtag = MCHUNK_TAG_FREE;
		mlist_add(&msegment->free_list[mclass], &mfree->list);
		msegment->num_free[mclass]++;

		offset_s += (1 << mclass);
	}
	Assert(offset_s == offset_e);
	pthread_mutex_unlock(&msegment->lock);

	mitem->mclass = mclass_new;

	return mitem->data;
}

void *
shmseg_resize(void *addr, size_t new_size)
{
	void   *result = shmseg_try_resize(addr, new_size);

	if (!result)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("pg_boost: out of shared memory")));
	return result;
}

void
shmseg_free(void *addr)
{
	mchunk_free_t  *mfree;
	mchunk_free_t  *buddy;
	uintptr_t		offset_mfree;
	uintptr_t		offset_buddy;
	int				mclass;

	mfree = (mchunk_free_t *)container_of(addr, mchunk_item_t, data);
	mclass = mfree->mclass;
	Assert(mfree->mtag != MCHUNK_TAG_FREE);

	pthread_mutex_lock(&msegment->lock);

	mfree->mtag = MCHUNK_TAG_FREE;
	msegment->num_active[mclass]--;
	msegment->segment_usage -= (1 << mclass);

	/*
	 * If its buddy is also free and same class, it shall be
	 * consolidated into one bigger free chunk
	 */
	offset_mfree = addr_to_offset(mfree);

	while (mclass < MSEGMENT_CLASS_MAX_BITS)
	{
		if (offset_mfree & (1 << mclass))
			offset_buddy = (offset_mfree & ~(1 << mclass));
		else
			offset_buddy = (offset_mfree | (1 << mclass));
		Assert((offset_buddy & ((1 << mclass) - 1)) == 0);

		/*
		 * buddy must be exist in the shared memory segment
		 */
		if (offset_buddy < sizeof(msegment_t) ||
			offset_buddy > msegment->segment_size)
			break;

		/*
		 * Also free? and same class?
		 */
		if (buddy->mclass != mclass || buddy->mtag != MCHUNK_TAG_FREE)
			break;

		/*
		 * Consolidate them
		 */
		mlist_del(&buddy->list);
		msegment->num_free[mclass]--;

		mclass++;
		offset_mfree &= ~((1 << mclass) - 1);
		mfree = offset_to_addr(offset_mfree);
		mfree->mclass = mclass;
		mfree->mtag = MCHUNK_TAG_FREE;
	}

	/*
	 * Attach this chunk to free_list[mclass]
	 */
	mlist_add(&msegment->free_list[mclass], &mfree->list);
	msegment->num_free[mclass]++;

	pthread_mutex_unlock(&msegment->lock);
}

size_t
shmseg_get_size(void *addr)
{
	mchunk_item_t  *mitem = container_of(addr, mchunk_item_t, data);

	return (1 << mitem->mclass) - offsetof(mchunk_item_t, data);
}

mchunk_buffer_t *
shmseg_alloc_buffer(size_t size)
{}
void
shmseg_free_buffer(mchunk_buffer_t *mbuffer);

void *
shmseg_get_read_buffer(mchunk_buffer_t *mbuffer)
{}

void *
shmseg_get_write_buffer(mchunk_buffer_t *mbuffer)
{

}

void
shmseg_put_buffer(mbuffer_t mbuffer, bool is_dirty)
{}

void
shmseg_init_mutex(pthread_mutex_t *lock)
{
	if (pthread_mutex_init(lock, &msegment_mutex_attr) != 0)
		elog(ERROR, "Failed to initialize mutex object : %m");
}

void
shmseg_init_rwlock(pthread_rwlock_t *lock)
{
	if (pthread_rwlock_init(lock, &msegment_rwlock_attr) != 0)
		elog(ERROR, "Failed to initialize rwlock object : %m");
}

static void
shmseg_init_msegment(int shmid, size_t segment_size, size_t buffer_size)
{
	uintptr_t	offset;
	int			mclass;
	int			index;

	msegment->shmid = shmid;
	msegment->segment_size = segment_size;
	msegment->segment_usage = 0;
	for (index = 0; index <= MSEGMENT_CLASS_MAX_BITS; index++)
	{
		mlist_init(&msegment->free_list[index]);
		msegment->num_active[index] = 0;
		msegment->num_free[index] = 0;
	}
	shmseg_init_mutex(&msegment->lock);

	offset = 1 << (fast_fls(sizeof(msegment_t)) + 1);
	if (offset < MSEGMENT_CLASS_MIN_SIZE)
		offset = MSEGMENT_CLASS_MIN_SIZE;

	while (segment_size >= offset + MSEGMENT_CLASS_MIN_SIZE)
	{
		mchunk_free_t  *mfree;

		/* choose an appropriate chunk class */
		mclass = fast_ffs(offset);
		if (mclass >= MSEGMENT_CLASS_MAX_BITS)
			mclass = MSEGMENT_CLASS_MAX_BITS;
		Assert(mclass >= MSEGMENT_CLASS_MIN_BITS);

		/* if (offset + chunk size) over the tail, truncate it */
		while (segment_size < offset + (1 << mclass))
			mclass--;

		if (mclass < MSEGMENT_CLASS_MIN_BITS)
			break;

		mfree = (mchunk_free_t *)offset_to_addr(offset);
		mfree->mclass = mclass;
		mfree->mtag = MCHUNK_TAG_FREE;
		mlist_add(&msegment->free_list[mclass], &mfree->list);

		msegment->num_free[mclass]++;

		offset += (1 << mclass);
	}

	for (index = 0; index < MSEGMENT_BUFFER_NUM_SLOTS; index++)
	{
		mlist_init(&msegment->mbuffer_list[index]);
		shmseg_init_mutex(&msegment->mbuffer_lock[index]);
	}
	msegment->mbuffer_index = 0;
	msegment->mbuffer_reclaim = 0;
	msegment->mbuffer_size = buffer_size;
	msegment->mbuffer_usage = 0;
}

static void
shmseg_init_guc_variables(int  *guc_segment_size,
						  int  *guc_buffer_size,
						  bool *guc_with_hugetlb)
{
	DefineCustomIntVariable("pg_boost.segment_size",
							"Size of shared memory segment in MB",
							NULL,
							guc_segment_size,
							128,			/* 128MB */
							32,				/*  32MB */
							4192 * 1024,	/* 4TB */
							PGC_SIGHUP,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	DefineCustomIntVariable("pg_boost.buffer_size",
							"Size of uncompressed buffer in MB",
							NULL,
							guc_buffer_size,
							*guc_segment_size * 60 / 100,	/* 60% */
							*guc_segment_size *  5 / 100,	/*  5% */
							*guc_segment_size * 95 / 100,	/* 95% */
							PGC_SIGHUP,
							GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("pg_boost.with_hugetlb",
							 "True, if HugeTlb on shared memory segment",
							 NULL,
							 guc_with_hugetlb,
							 false,
							 PGC_SIGHUP,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);
}

void
shmseg_init(void)
{
	static int	guc_segment_size;
	static int	guc_buffer_size;
	static bool	guc_with_hugetlb;
	size_t		segment_size;
	size_t		buffer_size;
	int			shmid;
	int			shmflags;

	/*
	 * Init mutex objects
	 */
	if (pthread_mutexattr_init(&msegment_mutex_attr) != 0 ||
		pthread_mutexattr_setpshared(&msegment_mutex_attr,
									 PTHREAD_PROCESS_SHARED))
		elog(ERROR, "failed on init mutex attribute");

	if (pthread_rwlockattr_init(&msegment_rwlock_attr) != 0 ||
		pthread_rwlockattr_setpshared(&msegment_rwlock_attr,
									  PTHREAD_PROCESS_SHARED))
		elog(ERROR, "failed on init rwlock attribute");

	/*
	 * Init GUC variables
	 */
	shmseg_init_guc_variables(&guc_segment_size,
							  &guc_buffer_size,
							  &guc_with_hugetlb);
	segment_size = (guc_segment_size << 20);
	buffer_size = (guc_buffer_size << 20);

	/*
	 * Create and map shared memory segment according to the configured
	 * parameters. The shmctl(IPC_RMID) shall be invoked to ensure
	 * deletion of the shared memory segment after process crashes, but
	 * it is not destroyed as long as a process maps it at least.
	 */
	shmflags = 0600 | IPC_CREAT | IPC_EXCL;
	if (guc_with_hugetlb)
		shmflags |= SHM_HUGETLB;

	shmid = shmget(IPC_PRIVATE, segment_size, shmflags);
	if (shmid < 0)
		elog(ERROR, "could not create a shared memory segment: %m");

	msegment = shmat(shmid, NULL, 0);

	shmctl(shmid, IPC_RMID, NULL);

	if (msegment == (void *)(-1))
		elog(ERROR, "could not attach a shared memory segment: %m");

	PG_TRY();
	{
		shmseg_init_msegment(shmid, segment_size, buffer_size);
	}
	PG_CATCH();
	{
		shmdt(msegment);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

void
shmseg_exit(void)
{
	shmdt(msegment);
}
