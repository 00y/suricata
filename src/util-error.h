/** Copyright (c) 2009 Open Information Security Foundation.
 *  \author Anoop Saldanha <poonaatsoc@gmail.com>
 */

#ifndef __ERROR_H__
#define __ERROR_H__


/* different error types */
typedef enum {
    SC_OK,
    SC_ERR_MEM_ALLOC,
    SC_PCRE_MATCH_FAILED,
    SC_LOG_MODULE_NOT_INIT,
    SC_LOG_FG_FILTER_MATCH_FAILED,
    SC_COUNTER_EXCEEDED,
    SC_INVALID_CHECKSUM,
    SC_SPRINTF_ERROR,
} SCError;

const char *SCErrorToString(SCError);


#endif /* __ERROR_H__ */
