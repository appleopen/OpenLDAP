/* operation.c - routines to deal with pending ldap operations */
/* $OpenLDAP: pkg/ldap/servers/slapd/operation.c,v 1.25 2002/01/26 12:57:41 hyc Exp $ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"


void
slap_op_free( Operation *op )
{
	assert( LDAP_STAILQ_NEXT(op, o_next) == NULL );

	if ( op->o_ber != NULL ) {
		ber_free( op->o_ber, 1 );
	}
	if ( op->o_dn.bv_val != NULL ) {
		free( op->o_dn.bv_val );
	}
	if ( op->o_ndn.bv_val != NULL ) {
		free( op->o_ndn.bv_val );
	}
	if ( op->o_authmech.bv_val != NULL ) {
		free( op->o_authmech.bv_val );
	}
	if ( op->o_ctrls != NULL ) {
		ldap_controls_free( op->o_ctrls );
	}

	ldap_pvt_thread_mutex_destroy( &op->o_abandonmutex );

	free( (char *) op );
}

Operation *
slap_op_alloc(
    BerElement		*ber,
    ber_int_t	msgid,
    ber_tag_t	tag,
    ber_int_t	id
)
{
	Operation	*op;

	op = (Operation *) ch_calloc( 1, sizeof(Operation) );

	ldap_pvt_thread_mutex_init( &op->o_abandonmutex );

	op->o_ber = ber;
	op->o_msgid = msgid;
	op->o_tag = tag;

	op->o_time = slap_get_time();
	op->o_opid = id;

	return( op );
}

#if 0
int slap_op_add(
    Operation		**olist,
	Operation		*op
)
{
	Operation	**tmp;

	for ( tmp = olist; *tmp != NULL; tmp = &(*tmp)->o_next )
		;	/* NULL */

	*tmp = op;

	return 0;
}

int
slap_op_remove( Operation **olist, Operation *op )
{
	Operation	**tmp;

	for ( tmp = olist; *tmp != NULL && *tmp != op; tmp = &(*tmp)->o_next )
		;	/* NULL */

	if ( *tmp == NULL ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_ERR,
			   "slap_op_remove: can't find op %ld.\n",
			   (long)op->o_msgid ));
#else
		Debug( LDAP_DEBUG_ANY, "op_delete: can't find op %ld\n",
		       (long) op->o_msgid, 0, 0 );
#endif

		return -1; 
	}

	*tmp = (*tmp)->o_next;
	op->o_next = NULL;

	return 0;
}

Operation * slap_op_pop( Operation **olist )
{
	Operation *tmp = *olist;

	if(tmp != NULL) {
		*olist = tmp->o_next;
		tmp->o_next = NULL;
	}

	return tmp;
}
#endif