/*-------------------------------------------------------------------------
 *
 * pg_stat_errors.c
 *		statistics of erros across a whole database cluster.
 *
 * Copyright (c) 2021, Alexey E. Konorev <alexey.konorev@gmail.com>
 *
 * Portions Copyright (c) 2008-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  pg_stat_errors/pg_stat_errors.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <time.h>

#include "access/hash.h"
#include "access/htup_details.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/syscache.h"	/* for check the database and role exists */
#include "utils/builtins.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* Location of permanent stats file (valid when database is shut down) */
#define PGSE_DUMP_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_stat_errors.stat"

/* Magic number identifying the stats file format */
static const uint32 PGSE_FILE_HEADER = 0x20210909;

/* PostgreSQL major version number, changes in which invalidate all entries */
static const uint32 PGSE_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

/* PGSE */
#define PGSE_DEALLOC_PERCENT       5    /* free this % of entries at once */
#define SQLSTATE_LEN              20
#define ERROR_MESSAGE_LEN        160
#define MAX_QUERY_LEN           1024


/*
 * Hashtable key that defines the identity of a hashtable entry. We separate
 * calls by user, by database and error code
 *
 */
typedef struct pgseHashKey
{
	Oid             userid;         /* user OID */
	Oid             dbid;           /* database OID */
	int             elevel;         /* error level */
	int             ecode;          /* error state */
} pgseHashKey;


/*
 * The actual stats counters kept within pgseEntry.
 */
typedef struct Counters
{
	int64           errors;         /* all errors except cancel and terminate */
	/* internal usage */
	TimestampTz     _first_change;
	TimestampTz     _last_change;   /* also use as last_time column */
} Counters;


/*
 * The last errors kept within pgsqEntryError
 */
typedef struct ErrorInfo
{
	TimestampTz     etime;                          /* timestamp of error */
	Oid             userid;                         /* user OID */
	Oid             dbid;                           /* database OID */
	char            query[MAX_QUERY_LEN];
	int             elevel;                         /* error level */
	int             ecode;                          /* encoded ERRSTATE */
	char            message[ERROR_MESSAGE_LEN];     /* primary error message (translated) */
} ErrorInfo;


/*
 * Global statistics for pg_stat_errors
 */
typedef struct pgseGlobalStats
{
	int64           dealloc;        /* # of times entries were deallocated */
	TimestampTz     stats_reset;    /* timestamp with all stats reset */
} pgseGlobalStats;

typedef struct pgseGlobalEID
{
	uint32          curr_eid;       /* current index of errors array */
	uint32          next_eid;       /* next index of errors array */
	uint32          max_eid;        /* actual number of last errors if the number
	                                 * of errors is less than pgse_max_last */
} pgseGlobalEID;

/*
 * Statistics per key
 *
 */
typedef struct pgseEntry
{
	pgseHashKey     key;            /* hash key of entry - MUST BE FIRST */
	Counters        counters;       /* the statistics for this key */
	slock_t         mutex;          /* protects the counters only */
} pgseEntry;

/*
 * Last Errors. The simple ring buffer.
 */
typedef struct pgseEntryError
{
	ErrorInfo       error;          /* the error for this key */
	slock_t         mutex;          /* protects the error only */
} pgseEntryError;

/*
 * Global shared state
 */
typedef struct pgseSharedState
{
	LWLock          *lock;          /* protects hashtable search/modification */
	slock_t         mutex;          /* protects following fields only: */
	int64           total_errors;
	int64           last_skipped;
	pgseGlobalEID   eid;
	pgseGlobalStats stats;          /* global statistics for pgse */
} pgseSharedState;


/*---- Local variables ----*/
static bool sysinit = false;

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static emit_log_hook_type prev_emit_log_hook = NULL;

/* Links to shared memory state */
static pgseSharedState *pgse = NULL;
static HTAB *pgse_hash = NULL;
static pgseEntryError *pgse_errors = NULL;

/*---- GUC variables ----*/
static int      pgse_max;               /* max # errors type to track */
static int      pgse_max_last;          /* max # of last errors */
static bool     pgse_save;              /* whether to save stats across shutdown */

#define _snprintf(_str_dst, _str_src, _len, _max_len)\
	memcpy((void *)_str_dst, _str_src, _len < _max_len ? _len : _max_len)

#define isInitialized() \
	( pgse && pgse_hash && pgse_errors && sysinit )

#define pgse_reset() \
	do { \
		volatile pgseSharedState *s = (volatile pgseSharedState *) pgse; \
		SpinLockAcquire(&s->mutex); \
		s->last_skipped = 0; \
		s->total_errors = 0; \
		s->eid.curr_eid = 0; \
		s->eid.next_eid = 0; \
		s->eid.max_eid = 0; \
		s->stats.dealloc = 0; \
		s->stats.stats_reset = GetCurrentTimestamp(); \
		SpinLockRelease(&s->mutex); \
	} while(0)

/*---- Function declarations ----*/

void _PG_init(void);
void _PG_fini(void);

PG_FUNCTION_INFO_V1(pg_stat_errors_reset);
PG_FUNCTION_INFO_V1(pg_stat_errors);
PG_FUNCTION_INFO_V1(pg_stat_errors_total_errors);
PG_FUNCTION_INFO_V1(pg_stat_errors_info);
PG_FUNCTION_INFO_V1(pg_stat_errors_glevel);
PG_FUNCTION_INFO_V1(pg_stat_errors_gcode);
PG_FUNCTION_INFO_V1(pg_stat_errors_gmessage);
PG_FUNCTION_INFO_V1(pg_stat_errors_last);

static void pgse_shmem_startup(void);
static void pgse_shmem_shutdown(int code, Datum arg);
static void pgse_emit_log_hook(ErrorData *edata);

#if (PG_VERSION_NUM < 90600)
static uint32 pgse_hash_fn(const void *key, Size keysize);
static int pgse_match_fn(const void *key1, const void *key2, Size keysize);
#endif
static Size pgse_memsize(void);
static pgseEntry *entry_alloc(pgseHashKey *key);
static void entry_dealloc(void);
static void entry_reset(void);
static void pgse_store(const TimestampTz etime, const char *query, const ErrorData *edata);
static void pgse_store_errorinfo(const ErrorInfo *eInfo);


/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_errors functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("pg_stat_errors.max",
	                        "Sets the maximum number of error types.",
	                        NULL,
	                        &pgse_max,
	                        100,
	                        10,
	                        INT_MAX,
	                        PGC_POSTMASTER,
	                        0,
	                        NULL,
	                        NULL,
	                        NULL);

	DefineCustomIntVariable("pg_stat_errors.max_last",
	                        "Sets the maximum number of last errors.",
	                        NULL,
	                        &pgse_max_last,
	                        20,
	                        10,
	                        INT_MAX,
	                        PGC_POSTMASTER,
	                        0,
	                        NULL,
	                        NULL,
	                        NULL);

	DefineCustomBoolVariable("pg_stat_errors.save",
	                         "Save pg_stat_errors statistics across server shutdowns.",
	                         NULL,
	                         &pgse_save,
	                         true,
	                         PGC_SIGHUP,
	                         0,
	                         NULL,
	                         NULL,
	                         NULL);

	EmitWarningsOnPlaceholders("pg_stat_errors");

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in pgse_shmem_startup().
	 */
	RequestAddinShmemSpace(pgse_memsize());
#if (PG_VERSION_NUM >= 90600)
	RequestNamedLWLockTranche("pg_stat_errors", 1);
#else
	RequestAddinLWLocks(1);
#endif

	/*
	 * Install hooks.
	 */
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgse_shmem_startup;
	prev_emit_log_hook = emit_log_hook;
	emit_log_hook = pgse_emit_log_hook;

	sysinit = true;
}


/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	sysinit = false;

	/* Uninstall hooks. */
	shmem_startup_hook = prev_shmem_startup_hook;
	emit_log_hook = prev_emit_log_hook;
}


/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 */
static void
pgse_shmem_startup(void)
{
	bool            found;
	HASHCTL         info;
	FILE            *file = NULL;
	uint32          header;
	int32           num;
	int32           num_last;
	int32           pgver;
	int32           i;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgse = NULL;
	pgse_hash = NULL;
	pgse_errors = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgse = ShmemInitStruct("pg_stat_errors",
	                       sizeof(pgseSharedState),
	                       &found);

	if (!found)
	{
		/* First time through ... */
#if (PG_VERSION_NUM >= 90600)
		pgse->lock = &(GetNamedLWLockTranche("pg_stat_errors"))->lock;
#else
		pgse->lock = LWLockAssign();
#endif
		SpinLockInit(&pgse->mutex);
		pgse_reset();
	}

	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(pgseHashKey);
	info.entrysize = sizeof(pgseEntry);
#if (PG_VERSION_NUM < 90600)
	info.hash = pgse_hash_fn;
	info.match = pgse_match_fn;

	pgse_hash = ShmemInitHash("pg_stat_errors hash",
	                          pgse_max, pgse_max,
	                          &info,
	                          HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
#else
        pgse_hash = ShmemInitHash("pg_stat_errors hash",
                                  pgse_max, pgse_max,
                                  &info,
                                  HASH_ELEM | HASH_BLOBS);
#endif
	{
		int arr_size = sizeof(pgseEntryError) * pgse_max_last;

		pgse_errors = (pgseEntryError *) ShmemAlloc(arr_size);
		/* Mark array as empty */
		memset(pgse_errors, 0, arr_size);
	}

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgse_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found)
		return;

	/*
	 * not try to unlink any old dump file in this case.  This seems a bit
	 * questionable but it's the historical behavior.)
	 */
	if (!pgse_save)
		return;

	/*
	 * Attempt to load old statistics from the dump file.
	 */
	file = AllocateFile(PGSE_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		return;
	}

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
	    fread(&pgver, sizeof(uint32), 1, file) != 1 ||
	    fread(&(pgse->total_errors), sizeof(uint64), 1, file) != 1 ||
	    fread(&num, sizeof(int32), 1, file) != 1
	   )
		goto read_error;

	if (header != PGSE_FILE_HEADER ||
	    pgver != PGSE_PG_MAJOR_VERSION
	   )
		goto data_error;

	/* load statistics of errors */
	for (i = 0; i < num; i++)
	{
		pgseEntry   temp;
		pgseEntry   *entry;

		if (fread(&temp, sizeof(pgseEntry), 1, file) != 1)
			goto read_error;

		/* Skip loading "sticky" entries */
		if (temp.counters.errors == 0)
			continue;

		/* make the hashtable entry (discards old entries if too many) */
		entry = entry_alloc(&temp.key);

		/* copy in the actual stats */
		if (entry)
			entry->counters = temp.counters;
	}

	/* Read global statistics for pg_stat_errors */
	if (fread(&pgse->stats, sizeof(pgseGlobalStats), 1, file) != 1)
		goto read_error;

	/* load last errors */
	if (fread(&num_last, sizeof(int32), 1, file) != 1)
		goto read_error;

	for (i = 0; i < num_last; i++)
	{
		ErrorInfo   temp;

		if (fread(&temp, sizeof(ErrorInfo), 1, file) != 1)
			goto read_error;

		pgse_store_errorinfo(&temp);
	}


	FreeFile(file);

	/*
	 * Remove the persisted stats file so it's not included in
	 * backups/replication slaves, etc.  A new file will be written on next
	 * shutdown.
	 */
	unlink(PGSE_DUMP_FILE);

	return;

read_error:
	ereport(LOG,
	        (errcode_for_file_access(),
	         errmsg("could not read pg_stat_errors file \"%s\": %m",
	                           PGSE_DUMP_FILE)));
	goto fail;
data_error:
	ereport(LOG,
	        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
	         errmsg("ignoring invalid data in pg_stat_errors file \"%s\"",
	                           PGSE_DUMP_FILE)));
fail:
	if (file)
		FreeFile(file);

	/* If possible, throw away the bogus file; ignore any error */
	unlink(PGSE_DUMP_FILE);
}


/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
pgse_shmem_shutdown(int code, Datum arg)
{
	FILE             *file;
	HASH_SEQ_STATUS  hash_seq;
	int32            num_entries;
	int32            j, num_last, rbuf_indx;
	pgseEntry        *entry;

	/* Don't try to dump during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if ( !isInitialized() )
		return;

	/* Don't dump if told not to. */
	if (!pgse_save)
		return;

	file = AllocateFile(PGSE_DUMP_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;
	if (fwrite(&PGSE_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;
	if (fwrite(&PGSE_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;
	if (fwrite(&(pgse->total_errors), sizeof(uint64), 1, file) != 1)
		goto error;

	/* save statistics of errors */
	num_entries = hash_get_num_entries(pgse_hash);
	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	hash_seq_init(&hash_seq, pgse_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (fwrite(entry, sizeof(pgseEntry), 1, file) != 1)
		{
			/* note: we assume hash_seq_term won't change errno */
			hash_seq_term(&hash_seq);
			goto error;
		}
	}

	/* Dump global statistics for pg_stat_errors */
	if (fwrite(&pgse->stats, sizeof(pgseGlobalStats), 1, file) != 1)
		goto error;

	/* save last errors */
	num_last = pgse->eid.max_eid;
	if (fwrite(&num_last, sizeof(int32), 1, file) != 1)
		goto error;

	/* store the latest errors in the order of occurrence */
	if ( (pgse->eid.max_eid != pgse_max_last) ||
	     (  (pgse->eid.max_eid == pgse_max_last) &&
	        ((pgse->eid.curr_eid + 1) == pgse->eid.max_eid)  )
	   )
		rbuf_indx = 0;
	else
		rbuf_indx = pgse->eid.curr_eid + 1;

	for (j=0; j<num_last; j++)
	{
		if (fwrite(&pgse_errors[rbuf_indx].error, sizeof(ErrorInfo), 1, file) != 1)
			goto error;

		rbuf_indx++;

		if (rbuf_indx >= pgse_max_last)
			rbuf_indx = 0;
	}


	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

#if (PG_VERSION_NUM >= 90407)
	/*
	 * Rename file into place, so we atomically replace any old one.
	 */
	(void) durable_rename(PGSE_DUMP_FILE ".tmp", PGSE_DUMP_FILE, LOG);
#else
	/*
	 * Rename file inplace
	 */
	if (rename(PGSE_DUMP_FILE ".tmp", PGSE_DUMP_FILE) != 0)
		ereport(LOG,
		        (errcode_for_file_access(),
		         errmsg("could not rename pg_stat_errors file \"%s\": %m",
		                        PGSE_DUMP_FILE ".tmp")));
#endif

	return;

error:
	ereport(LOG,
	        (errcode_for_file_access(),
	         errmsg("could not write pg_stat_errors file \"%s\": %m",
	                                PGSE_DUMP_FILE ".tmp")));
	if (file)
		FreeFile(file);
	unlink(PGSE_DUMP_FILE ".tmp");
}


/*
 * Error hook.
 */
void
pgse_emit_log_hook(ErrorData *edata)
{
	if ( !isInitialized() || !edata )
		goto exit;

	if (edata->elevel >= WARNING)
	{
		pgse_store(GetCurrentTimestamp(), 
		           debug_query_string ? debug_query_string : "",
		           edata);
	}
exit:
	if (prev_emit_log_hook)
		prev_emit_log_hook(edata);
}


#if (PG_VERSION_NUM < 90600)
/*
 * Calculate hash value for a key
 */
static uint32
pgse_hash_fn(const void *key, Size keysize)
{
	const pgseHashKey *k = (const pgseHashKey *) key;

	return hash_uint32((uint32) k->userid) ^
	       hash_uint32((uint32) k->dbid) ^
	       hash_uint32((uint32) k->elevel) ^
	       hash_uint32((uint32) k->ecode);
}

/*
 * Compare two keys - zero means match
 */
static int
pgse_match_fn(const void *key1, const void *key2, Size keysize)
{
	const pgseHashKey *k1 = (const pgseHashKey *) key1;
	const pgseHashKey *k2 = (const pgseHashKey *) key2;

	if (k1->userid == k2->userid &&
	    k1->dbid == k2->dbid &&
	    k1->elevel == k2->elevel &&
	    k1->ecode == k2->ecode
	   )
		return 0;
	else
		return 1;
}
#endif


/*
 * Estimate shared memory space needed.
 */
static Size
pgse_memsize(void)
{
	Size       size;

	size = MAXALIGN(sizeof(pgseSharedState));
	size = add_size(size, hash_estimate_size(pgse_max, sizeof(pgseEntry)));
	size += MAXALIGN(sizeof(pgseEntryError)*pgse_max_last);

	elog(DEBUG1, "pg_stat_errors: %s(): SharedState: [%lu] Entries: [%lu] EntryErrors: [%lu] total: [%lu] ", __FUNCTION__,
	        sizeof(pgseSharedState), hash_estimate_size(pgse_max, sizeof(pgseEntry)), sizeof(pgseEntryError)*pgse_max_last, size);

	return size;
}


/*
 * Allocate a new hashtable entry.
 * Caller must hold an exclusive lock on pgse->lock
 * 
 */
static pgseEntry *
entry_alloc(pgseHashKey *key)
{
	pgseEntry  *entry;
	bool       found;

	/* Make space if needed */
	while (hash_get_num_entries(pgse_hash) >= pgse_max)
		entry_dealloc();

	/* Find or create an entry with desired hash code */
	entry = (pgseEntry *) hash_search(pgse_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */

		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(Counters));
		/* marking the first change */
		entry->counters._first_change = GetCurrentTimestamp();
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
	}

	return entry;
}


/*
 * qsort comparator for sorting into increasing timestamp order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	TimestampTz  l_ts = (*(pgseEntry *const *) lhs)->counters._last_change;
	TimestampTz  r_ts = (*(pgseEntry *const *) rhs)->counters._last_change;

	/* first field - timestamp */
	if (l_ts < r_ts)
		return -1;
	else if (l_ts > r_ts)
		return +1;
	else
		return 0;
}


/*
 * Deallocate most oldest entries.
 *
 * Caller must hold an exclusive lock on pgse->lock.
 */
static void
entry_dealloc(void)
{
	HASH_SEQ_STATUS  hash_seq;
	pgseEntry        **entries;
	pgseEntry        *entry;
	int              nvictims;
	int              i;

	/*
	 * Sort entries by last error and deallocate PGSE_DEALLOC_PERCENT of them.
	 */

	entries = palloc(hash_get_num_entries(pgse_hash) * sizeof(pgseEntry *));

	i = 0;

	hash_seq_init(&hash_seq, pgse_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
	}

	/* Sort into increasing order by last time */
	qsort(entries, i, sizeof(pgseEntry *), entry_cmp);

	/* Now zap an appropriate fraction of lowest-time entries */
	nvictims = Max(2, i * PGSE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(pgse_hash, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);

	/* Increment the number of times entries are deallocated */
	{
		volatile pgseSharedState *s = (volatile pgseSharedState *) pgse;

		SpinLockAcquire(&s->mutex);
		s->stats.dealloc += 1;
		SpinLockRelease(&s->mutex);
	}
}


/*
 * Release all entries.
 */
static void
entry_reset(void)
{
	HASH_SEQ_STATUS  hash_seq;
	pgseEntry         *entry;

	LWLockAcquire(pgse->lock, LW_EXCLUSIVE);

	hash_seq_init(&hash_seq, pgse_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(pgse_hash, &entry->key, HASH_REMOVE, NULL);
	}

	LWLockRelease(pgse->lock);
}

/*
 * Release all last errors.
 */
static void
errors_reset(void)
{
	LWLockAcquire(pgse->lock, LW_EXCLUSIVE);
	memset(pgse_errors, 0, (sizeof(pgseEntryError) * pgse_max_last));
	LWLockRelease(pgse->lock);
}

/*
 * Reset all statistics of errors
 */
Datum
pg_stat_errors_reset(PG_FUNCTION_ARGS)
{
	if ( !isInitialized() )
		ereport(ERROR,
		        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		         errmsg("pg_stat_errors must be loaded via shared_preload_libraries")));
	entry_reset();
	errors_reset();
	pgse_reset();
	PG_RETURN_VOID();
}


/*
 * Store last errors
 *
 * Caller must hold an exclusive lock on pgse->lock.
 */
static void
pgse_store_error(const TimestampTz etm, const Oid dbid, const Oid userid, const char *query, const ErrorData *edata)
{
	pgse->eid.curr_eid = pgse->eid.next_eid;
	pgse->eid.next_eid++;

	if (pgse->eid.next_eid >= pgse_max_last)
		pgse->eid.next_eid = 0;

	if (pgse->eid.max_eid < pgse_max_last)
		pgse->eid.max_eid++;

	/* volatile block */
	{
		int message_len = strlen (edata->message);
		int query_len = strlen (query);
		volatile pgseEntryError *e = (volatile pgseEntryError *) &pgse_errors[pgse->eid.curr_eid];

		SpinLockAcquire(&e->mutex);

		/* reset old error */
		memset(&pgse_errors[pgse->eid.curr_eid].error, 0, sizeof(ErrorInfo));

		e->error.etime = etm;
		e->error.userid = userid;
		e->error.dbid = dbid;
		e->error.elevel = edata->elevel;
		e->error.ecode = edata->sqlerrcode;
		_snprintf(e->error.query, query, query_len, MAX_QUERY_LEN);
		_snprintf(e->error.message, edata->message, message_len, ERROR_MESSAGE_LEN);

		SpinLockRelease(&e->mutex);
	}
}

static void
pgse_store_errorinfo(const ErrorInfo *eInfo)
{
	pgse->eid.curr_eid = pgse->eid.next_eid;
	pgse->eid.next_eid++;

	if (pgse->eid.next_eid >= pgse_max_last)
		pgse->eid.next_eid = 0;

	if (pgse->eid.max_eid < pgse_max_last)
		pgse->eid.max_eid++;

	/* volatile block */
	{
		volatile pgseEntryError *e = (volatile pgseEntryError *) &pgse_errors[pgse->eid.curr_eid];

		SpinLockAcquire(&e->mutex);

		/* reset old error */
		memset(&pgse_errors[pgse->eid.curr_eid].error, 0, sizeof(ErrorInfo));
		memcpy((void *)&e->error, eInfo, sizeof(ErrorInfo));

		SpinLockRelease(&e->mutex);
	}
}


/*
 * Update counters
 *
 * Caller must hold an exclusive lock on pgse->lock.
 */
static void
pgse_update_counters(const TimestampTz etm, const pgseEntry *entry, const ErrorData *edata)
{
	/* volatile block */
	{	
		/*
		 * Grab the spinlock while updating the counters (see comment about
		 * locking rules at the head of the file)
		 */
		volatile pgseEntry *e = (volatile pgseEntry *) entry;

		SpinLockAcquire(&e->mutex);

		if (edata->sqlerrcode != ERRCODE_SUCCESSFUL_COMPLETION)
		{
			e->counters.errors++;
		}
		e->counters._last_change = etm;

		SpinLockRelease(&e->mutex);
	}

	pgse->total_errors++;
}

/*
 * Store some statistics for key and whole database cluster
 */
static void
pgse_store(const TimestampTz etm, const char *query, const ErrorData *edata)
{
	pgseHashKey      key;
	pgseEntry        *entry;

	/* Safety check ... */
	if ( !isInitialized() || !edata )
		return;

	/* Set up key for hashtable search */
	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.elevel = edata->elevel;
	key.ecode = edata->sqlerrcode;

	/* Lookup the hash table entry with shared lock. */
	LWLockAcquire(pgse->lock, LW_SHARED);

	entry = (pgseEntry *) hash_search(pgse_hash, &key, HASH_FIND, NULL);

	/* Create new entry, if not present */
	if (!entry)
	{
		/* Need exclusive lock to make a new hashtable entry - promote */
		LWLockRelease(pgse->lock);
		LWLockAcquire(pgse->lock, LW_EXCLUSIVE);

		/* OK to create a new hashtable entry */
		entry = entry_alloc(&key);
	}

	pgse_update_counters(etm, entry, edata);
	/* store last errors */
	pgse_store_error(etm, key.dbid, key.userid, query, edata);

	LWLockRelease(pgse->lock);
}




/*
 * Total errors
 */
Datum
pg_stat_errors_total_errors(PG_FUNCTION_ARGS)
{
	int64 result;

	volatile pgseSharedState *s = (volatile pgseSharedState *) pgse;
	SpinLockAcquire(&s->mutex);
	result = s->total_errors;
	SpinLockRelease(&s->mutex);

	PG_RETURN_INT64(result);
}


/*
 * Get error code as text
 */
Datum
pg_stat_errors_gcode(PG_FUNCTION_ARGS)
{
	int     ecode = PG_GETARG_INT64(0);
	char    ecode_text[SQLSTATE_LEN] = {0};

	snprintf(ecode_text, SQLSTATE_LEN, "%s", unpack_sql_state(ecode));

	PG_RETURN_TEXT_P(cstring_to_text(ecode_text));
}

/*
 * Get error message
 */
Datum
pg_stat_errors_gmessage(PG_FUNCTION_ARGS)
{
	int     ecode = PG_GETARG_INT64(0);
	char    *result;

	switch (ecode) 
	{
#include "ecodes.inc"
		default:
			result = "unknown";
	}
    
	PG_RETURN_TEXT_P(cstring_to_text(result));
}


/*
 * Get error level as text
 * only WARNING, ERROR, FATAL, PANIC error level codes
 */
Datum
pg_stat_errors_glevel(PG_FUNCTION_ARGS)
{
	int     elevel = PG_GETARG_INT64(0);
	char    *elevel_text = NULL;

	switch (elevel)
	{
		case WARNING:
			elevel_text = "WARNING";
			break;
		case ERROR:
			elevel_text = "ERROR";
			break;
		case FATAL:
			elevel_text = "FATAL";
			break;
		case PANIC:
			elevel_text = "PANIC";
			break;
	}

	PG_RETURN_TEXT_P(cstring_to_text(elevel_text));
}


/* Number of output arguments (columns) for pg_stat_errors_info */
#define PG_STAT_ERRORS_INFO_COLS    2

/*
 * Return statistics of pg_stat_errors.
 */
Datum
pg_stat_errors_info(PG_FUNCTION_ARGS)
{
	pgseGlobalStats stats;
	TupleDesc       tupdesc;
	Datum           values[PG_STAT_ERRORS_INFO_COLS];
	bool            nulls[PG_STAT_ERRORS_INFO_COLS];

	if ( !isInitialized() )
		ereport(ERROR,
		        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		        errmsg("pg_stat_errors must be loaded via shared_preload_libraries")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	/* Read global statistics for pg_stat_errors */
	{
		volatile pgseSharedState *s = (volatile pgseSharedState *) pgse;

		SpinLockAcquire(&s->mutex);
		stats = s->stats;
		SpinLockRelease(&s->mutex);
	}

	values[0] = Int64GetDatum(stats.dealloc);
	values[1] = TimestampTzGetDatum(stats.stats_reset);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}


#define PG_STAT_ERRORS_COLS	7

/*
 * Retrieve statistics of errors per key
 */
Datum
pg_stat_errors(PG_FUNCTION_ARGS)
{
	ReturnSetInfo       *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc           tupdesc;
	Tuplestorestate     *tupstore;
	MemoryContext       per_query_ctx;
	MemoryContext       oldcontext;
	HASH_SEQ_STATUS     hash_seq;
	pgseEntry           *entry;

	/* hash table must exist already */
	if ( !isInitialized() )
		ereport(ERROR,
		        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		         errmsg("pg_stat_errors must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		         errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		         errmsg("materialize mode required, but it is not allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (tupdesc->natts != PG_STAT_ERRORS_COLS)
		elog(ERROR, "incorrect number of output arguments, required %d", tupdesc->natts);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * With a large hash table, we might be holding the lock rather longer
	 * than one could wish.  However, this only blocks creation of new hash
	 * table entries, and the larger the hash table the less likely that is to
	 * be needed.  So we can hope this is okay.  Perhaps someday we'll decide
	 * we need to partition the hash table to limit the time spent holding any
	 * one lock.
	 */
	LWLockAcquire(pgse->lock, LW_SHARED);

	hash_seq_init(&hash_seq, pgse_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum           values[PG_STAT_ERRORS_COLS];
		bool            nulls[PG_STAT_ERRORS_COLS];
		int             i = 0;
		Counters        tmp;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		values[i++] = Int64GetDatumFast(entry->key.elevel);
		values[i++] = Int64GetDatumFast(ERRCODE_TO_CATEGORY(entry->key.ecode));
		values[i++] = Int64GetDatumFast(entry->key.ecode);

		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgseEntry *e = (volatile pgseEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}
		values[i++] = Int64GetDatumFast(tmp.errors);
		values[i++] = TimestampTzGetDatum(tmp._last_change);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	/* clean up and return the tuplestore */
	LWLockRelease(pgse->lock);
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}


#define PG_STAT_ERRORS_LAST_COLS     7

/*
 * Retrieve last N errors
 */
Datum
pg_stat_errors_last(PG_FUNCTION_ARGS)
{
	ReturnSetInfo       *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc           tupdesc;
	Tuplestorestate     *tupstore;
	MemoryContext       per_query_ctx;
	MemoryContext       oldcontext;
	int                 j, num_last;

	/* array of errors must exist already */
	if ( !isInitialized() )
		ereport(ERROR,
		        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
		         errmsg("pg_stat_errors must be loaded via shared_preload_libraries")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		         errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
		        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		         errmsg("materialize mode required, but it is not allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (tupdesc->natts != PG_STAT_ERRORS_LAST_COLS)
		elog(ERROR, "incorrect number of output arguments, required %d", tupdesc->natts);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/*
	 * With a large array table, we might be holding the lock rather longer
	 * than one could wish. However, this only blocks creation of new array
	 * entries, and the larger the array the less likely that is to be needed.
	 * So we can hope this is okay.  Perhaps someday we'll decide we need to
	 * partition the array to limit the time spent holding any one lock.
	 */
	LWLockAcquire(pgse->lock, LW_SHARED);

	/* output only the actual number of errors if the number of errors
	 * is less than pgse_max_last */
	num_last = pgse->eid.max_eid;

	for (j=0; j<num_last; j++)
	{
		Datum           values[PG_STAT_ERRORS_LAST_COLS];
		bool            nulls[PG_STAT_ERRORS_LAST_COLS];
		int             i = 0;
		ErrorInfo       tmp;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgseEntryError *e = (volatile pgseEntryError *) &pgse_errors[j];

			SpinLockAcquire(&e->mutex);
			tmp = e->error;
			SpinLockRelease(&e->mutex);
		}
		values[i++] = TimestampTzGetDatum(tmp.etime);
		values[i++] = ObjectIdGetDatum(tmp.userid);
		values[i++] = ObjectIdGetDatum(tmp.dbid);
		values[i++] = CStringGetTextDatum(tmp.query);
		values[i++] = Int64GetDatumFast(tmp.elevel);
		values[i++] = Int64GetDatumFast(tmp.ecode);
		values[i++] = CStringGetTextDatum(tmp.message);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	LWLockRelease(pgse->lock);
	tuplestore_donestoring(tupstore);

	return (Datum)0;
}

