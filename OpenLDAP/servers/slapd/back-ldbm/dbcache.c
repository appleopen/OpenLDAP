/* ldbmcache.c - maintain a cache of open ldbm files */
/* $OpenLDAP: pkg/ldap/servers/slapd/back-ldbm/dbcache.c,v 1.47 2002/01/04 20:17:52 kurt Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/errno.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>
#include <ac/unistd.h>
#include <sys/stat.h>

#include "slap.h"
#include "back-ldbm.h"

DBCache *
ldbm_cache_open(
    Backend	*be,
    const char	*name,
    const char	*suffix,
    int		flags
)
{
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;
	int		i, lru, empty;
	time_t		oldtime;
	char		buf[MAXPATHLEN];
#ifdef HAVE_ST_BLKSIZE
	struct stat	st;
#endif

	if (li->li_envdirok)
		sprintf( buf, "%s%s", name, suffix );
	else
		sprintf( buf, "%s" LDAP_DIRSEP "%s%s",
			li->li_directory, name, suffix );

	if( li->li_dblocking ) {
		flags |= LDBM_LOCKING;
	} else {
		flags |= LDBM_NOLOCKING;
	}
	
	if( li->li_dbwritesync ) {
		flags |= LDBM_SYNC;
	} else {
		flags |= LDBM_NOSYNC;
	}
	
#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ENTRY,
		   "ldbm_cache_open: \"%s\", %d, %o\n", buf, flags, li->li_mode ));
#else
	Debug( LDAP_DEBUG_TRACE, "=> ldbm_cache_open( \"%s\", %d, %o )\n", buf,
	    flags, li->li_mode );
#endif


	empty = MAXDBCACHE;

	ldap_pvt_thread_mutex_lock( &li->li_dbcache_mutex );
	do {
		lru = 0;
		oldtime = 1;
		for ( i = 0; i < MAXDBCACHE; i++ ) {
			/* see if this slot is free */
			if ( li->li_dbcache[i].dbc_name == NULL) {
				if (empty == MAXDBCACHE)
					empty = i;
				continue;
			}

			if ( strcmp( li->li_dbcache[i].dbc_name, buf ) == 0 ) {
				/* already open - return it */
				if (li->li_dbcache[i].dbc_flags != flags
					&& li->li_dbcache[i].dbc_refcnt == 0)
				{
					/* we don't want to use an open cache with different
					 * permissions (esp. if we need write but the open
					 * cache is read-only).	 So close this one if
					 * possible, and re-open below.
					 *
					 * FIXME:  what about the case where the refcount
					 * is > 0?  right now, we're using it anyway and
					 * just praying.  Can there be more than one open
					 * cache to the same db?
					 *
					 * Also, it's really only necessary to compare the
					 * read-only flag, instead of all of the flags,
					 * but for now I'm checking all of them.
					 */
					lru = i;
					empty = MAXDBCACHE;
					break;
				}
				li->li_dbcache[i].dbc_refcnt++;
#ifdef NEW_LOGGING
				LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
					   "ldbm_cache_open: cache %d\n", i ));
#else
				Debug( LDAP_DEBUG_TRACE,
				    "<= ldbm_cache_open (cache %d)\n", i, 0, 0 );
#endif

				ldap_pvt_thread_mutex_unlock( &li->li_dbcache_mutex );
				return( &li->li_dbcache[i] );
			}

			/* keep track of lru db */
			if (( li->li_dbcache[i].dbc_refcnt == 0 ) &&
			      (( oldtime == 1 ) ||
			      ( li->li_dbcache[i].dbc_lastref < oldtime )) )
			{
				lru = i;
				oldtime = li->li_dbcache[i].dbc_lastref;
			}
		}

		i = empty;
		if ( i == MAXDBCACHE ) {
			/* no empty slots, not already open - close lru and use that slot */
			if ( li->li_dbcache[lru].dbc_refcnt == 0 ) {
				i = lru;
				ldbm_close( li->li_dbcache[i].dbc_db );
				free( li->li_dbcache[i].dbc_name );
				li->li_dbcache[i].dbc_name = NULL;
			} else {
#ifdef NEW_LOGGING
				LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
					   "ldbm_cache_open: no unused db to close - waiting\n" ));
#else
				Debug( LDAP_DEBUG_ANY,
				    "ldbm_cache_open no unused db to close - waiting\n",
				    0, 0, 0 );
#endif

				ldap_pvt_thread_cond_wait( &li->li_dbcache_cv,
					    &li->li_dbcache_mutex );
				/* after waiting for a free slot, go back to square
				 * one: look for an open cache for this db, or an
				 * empty slot, or an unref'ed cache, or wait again.
				 */
			}
		}
	} while (i == MAXDBCACHE);

	if ( (li->li_dbcache[i].dbc_db = ldbm_open( li->li_dbenv, buf, flags, li->li_mode,
	    li->li_dbcachesize )) == NULL )
	{
		int err = errno;
#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_ERR,
			   "ldbm_cache_open: \"%s\" failed, errono=%d, reason=%s\n",
			   buf, err, err > -1 && err < sys_nerr ? sys_errlist[err] :
			   "unknown" ));
#else
		Debug( LDAP_DEBUG_TRACE,
		    "<= ldbm_cache_open NULL \"%s\" errno=%d reason=\"%s\")\n",
		    buf, err, err > -1 && err < sys_nerr ?
		    sys_errlist[err] : "unknown" );
#endif

		ldap_pvt_thread_mutex_unlock( &li->li_dbcache_mutex );
		return( NULL );
	}
	li->li_dbcache[i].dbc_name = ch_strdup( buf );
	li->li_dbcache[i].dbc_refcnt = 1;
	li->li_dbcache[i].dbc_lastref = slap_get_time();
	li->li_dbcache[i].dbc_flags = flags;
	li->li_dbcache[i].dbc_dirty = 0;
#ifdef HAVE_ST_BLKSIZE
	if ( stat( buf, &st ) == 0 ) {
		li->li_dbcache[i].dbc_blksize = st.st_blksize;
	} else
#endif
	{
		li->li_dbcache[i].dbc_blksize = DEFAULT_BLOCKSIZE;
	}
	li->li_dbcache[i].dbc_maxids = (li->li_dbcache[i].dbc_blksize /
	    sizeof(ID)) - ID_BLOCK_IDS_OFFSET;
	li->li_dbcache[i].dbc_maxindirect = ( SLAPD_LDBM_MIN_MAXIDS /
	    li->li_dbcache[i].dbc_maxids ) + 1;

	assert( li->li_dbcache[i].dbc_maxindirect < 256 );

#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ARGS,
		   "ldbm_cache_open: blksize:%ld  maxids:%d  maxindirect:%d\n",
		   li->li_dbcache[i].dbc_blksize, li->li_dbcache[i].dbc_maxids,
		   li->li_dbcache[i].dbc_maxindirect ));
#else
	Debug( LDAP_DEBUG_ARGS,
	    "ldbm_cache_open (blksize %ld) (maxids %d) (maxindirect %d)\n",
	    li->li_dbcache[i].dbc_blksize, li->li_dbcache[i].dbc_maxids,
	    li->li_dbcache[i].dbc_maxindirect );
#endif

#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
		   "ldbm_cache_open: opened %d\n", i ));
#else
	Debug( LDAP_DEBUG_TRACE, "<= ldbm_cache_open (opened %d)\n", i, 0, 0 );
#endif

	ldap_pvt_thread_mutex_init( &li->li_dbcache[i].dbc_write_mutex );

	ldap_pvt_thread_mutex_unlock( &li->li_dbcache_mutex );
	return( &li->li_dbcache[i] );
}

void
ldbm_cache_close( Backend *be, DBCache *db )
{
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;

	if( li->li_dbwritesync && db->dbc_dirty ) {
		ldbm_sync( db->dbc_db );
		db->dbc_dirty = 0;
	}

	ldap_pvt_thread_mutex_lock( &li->li_dbcache_mutex );
	if ( --db->dbc_refcnt <= 0 ) {
		db->dbc_refcnt = 0;
		ldap_pvt_thread_cond_signal( &li->li_dbcache_cv );
	}
	ldap_pvt_thread_mutex_unlock( &li->li_dbcache_mutex );
}

void
ldbm_cache_really_close( Backend *be, DBCache *db )
{
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;

	ldap_pvt_thread_mutex_lock( &li->li_dbcache_mutex );
	if ( --db->dbc_refcnt <= 0 ) {
		db->dbc_refcnt = 0;
		ldap_pvt_thread_cond_signal( &li->li_dbcache_cv );
		ldbm_close( db->dbc_db );
		free( db->dbc_name );
		db->dbc_name = NULL;
		ldap_pvt_thread_mutex_destroy( &db->dbc_write_mutex );
	}
	ldap_pvt_thread_mutex_unlock( &li->li_dbcache_mutex );
}

void
ldbm_cache_flush_all( Backend *be )
{
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;
	int		i;

	ldap_pvt_thread_mutex_lock( &li->li_dbcache_mutex );
	for ( i = 0; i < MAXDBCACHE; i++ ) {
		if ( li->li_dbcache[i].dbc_name != NULL ) {
#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
				   "ldbm_cache_flush_all: flushing db (%s)\n",
				   li->li_dbcache[i].dbc_name ));
#else
			Debug( LDAP_DEBUG_TRACE, "ldbm flushing db (%s)\n",
			    li->li_dbcache[i].dbc_name, 0, 0 );
#endif

			ldbm_sync( li->li_dbcache[i].dbc_db );
			li->li_dbcache[i].dbc_dirty = 0;
			if ( li->li_dbcache[i].dbc_refcnt != 0 ) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
					   "ldbm_cache_flush_all: couldn't close db (%s), refcnt=%d\n",
					   li->li_dbcache[i].dbc_name, li->li_dbcache[i].dbc_refcnt ));
#else
				Debug( LDAP_DEBUG_TRACE,
				       "refcnt = %d, couldn't close db (%s)\n",
				       li->li_dbcache[i].dbc_refcnt,
				       li->li_dbcache[i].dbc_name, 0 );
#endif

			} else {
#ifdef NEW_LOGGING
				LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
					   "ldbm_cache_flush_all: ldbm closing db (%s)\n",
					   li->li_dbcache[i].dbc_name ));
#else
				Debug( LDAP_DEBUG_TRACE,
				       "ldbm closing db (%s)\n",
				       li->li_dbcache[i].dbc_name, 0, 0 );
#endif

				ldap_pvt_thread_cond_signal( &li->li_dbcache_cv );
				ldbm_close( li->li_dbcache[i].dbc_db );
				free( li->li_dbcache[i].dbc_name );
				li->li_dbcache[i].dbc_name = NULL;
			}
		}
	}
	ldap_pvt_thread_mutex_unlock( &li->li_dbcache_mutex );
}

void
ldbm_cache_sync( Backend *be )
{
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;
	int		i;

	ldap_pvt_thread_mutex_lock( &li->li_dbcache_mutex );
	for ( i = 0; i < MAXDBCACHE; i++ ) {
		if ( li->li_dbcache[i].dbc_name != NULL && li->li_dbcache[i].dbc_dirty ) {
			Debug(	LDAP_DEBUG_TRACE, "ldbm syncing db (%s)\n",
				li->li_dbcache[i].dbc_name, 0, 0 );
			ldbm_sync( li->li_dbcache[i].dbc_db );
			li->li_dbcache[i].dbc_dirty = 0;
		}
	}
	ldap_pvt_thread_mutex_unlock( &li->li_dbcache_mutex );
}

Datum
ldbm_cache_fetch(
    DBCache	*db,
    Datum		key
)
{
	return ldbm_fetch( db->dbc_db, key );
}

int
ldbm_cache_store(
    DBCache	*db,
    Datum		key,
    Datum		data,
    int			flags
)
{
	int	rc;

#ifdef LDBM_DEBUG
	Statslog( LDAP_DEBUG_STATS,
		"=> ldbm_cache_store(): key.dptr=%s, key.dsize=%d\n",
		key.dptr, key.dsize, 0, 0, 0 );

	Statslog( LDAP_DEBUG_STATS,
		"=> ldbm_cache_store(): key.dptr=0x%08x, data.dptr=0x%0 8x\n",
		key.dptr, data.dptr, 0, 0, 0 );

	Statslog( LDAP_DEBUG_STATS,
		"=> ldbm_cache_store(): data.dptr=%s, data.dsize=%d\n",
		data.dptr, data.dsize, 0, 0, 0 );

	Statslog( LDAP_DEBUG_STATS,
		"=> ldbm_cache_store(): flags=0x%08x\n",
		flags, 0, 0, 0, 0 );
#endif /* LDBM_DEBUG */

	db->dbc_dirty = 1;
	rc = ldbm_store( db->dbc_db, key, data, flags );

	return( rc );
}

int
ldbm_cache_delete(
    DBCache	*db,
    Datum		key
)
{
	int	rc;

	db->dbc_dirty = 1;
	rc = ldbm_delete( db->dbc_db, key );

	return( rc );
}

void *
ldbm_cache_sync_daemon(
	void *be_ptr
)
{
	Backend *be = (Backend *)be_ptr;
	struct ldbminfo	*li = (struct ldbminfo *) be->be_private;

	Debug( LDAP_DEBUG_ANY, "synchronizer starting for %s\n", li->li_directory, 0, 0 );
  
	while (!li->li_dbshutdown) {
		int i = li->li_dbsyncwaitn;

		sleep( li->li_dbsyncfreq );

		while (i && ldap_pvt_thread_pool_backload(&connection_pool) != 0) {
			Debug( LDAP_DEBUG_TRACE, "delay syncing %s\n", li->li_directory, 0, 0 );
			sleep(li->li_dbsyncwaitinterval);
			i--;
		}

		if (!li->li_dbshutdown) {
			Debug( LDAP_DEBUG_TRACE, "syncing %s\n", li->li_directory, 0, 0 );
			ldbm_cache_sync( be );
		}
	}

  	Debug( LDAP_DEBUG_ANY, "synchronizer stopping\n", 0, 0, 0 );
  
	return NULL;
}