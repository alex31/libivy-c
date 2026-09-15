/*
 *	Ivy, C interface
 *
 *	Copyright (C) 1997-2000
 *	Centre d'Études de la Navigation Aérienne
 *
 *	Bind syntax for extracting message comtent 
 *  using regexp or other 
 *
 *	Authors: François-Régis Colin <fcolin@cena.fr>
 *
 *	$Id: ivybind.c 3627 2015-01-07 14:01:47Z bustico $
 * 
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */
/* Module de gestion de la syntaxe des messages Ivy */
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <stdlib.h>
#include <memory.h> 
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#ifdef WIN32
#ifndef __MINGW32__
#include <crtdbg.h>
#endif
#endif


#ifdef USE_PCRE_REGEX
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#else  /* we don't USE_PCRE_REGEX */
#define MAX_MSG_FIELDS 200
#include <regex.h>
#endif /* USE_PCRE_REGEX */

#include "list.h"
#include "ivybind.h"
#include "ivy.h"
#include "ivythread.h"

static IVY_TLS char err_buf[4096];

struct _binding {
#ifdef USE_PCRE_REGEX
	pcre2_code *regexp;
	uint32_t capture_count;
#else  /* we don't USE_PCRE_REGEX */
	regex_t regexp;						/* la regexp sous forme machine */
	regmatch_t match[MAX_MSG_FIELDS+1];	/* resultat du match */
#endif /* USE_PCRE_REGEX */
	};

/* Filter lists belong to an IvyContext, protected by its bindings lock. */
struct _ivy_filter {
    struct _ivy_filter *next;
    char *word;
};

#ifdef USE_PCRE_REGEX
static IVY_TLS pcre2_match_data *thread_match_data = NULL;
static IVY_TLS uint32_t thread_capture_count = 0;
static IVY_TLS PCRE2_SIZE *thread_ovector = NULL;
static IVY_TLS int thread_nb_match = 0;
static IVY_TLS IvyBinding thread_last_bind = NULL;

static int IvyBindingPrepareMatchData(IvyBinding bind)
{
	uint32_t required_capture_count;

	required_capture_count = bind->capture_count + 1;
	if (thread_match_data != NULL && thread_capture_count >= required_capture_count)
		return 1;

	if (thread_match_data != NULL)
		pcre2_match_data_free(thread_match_data);

	thread_match_data = pcre2_match_data_create(required_capture_count, NULL);
	if (thread_match_data == NULL)
		return 0;

	thread_capture_count = required_capture_count;
	return 1;
}
#endif /* USE_PCRE_REGEX */

int IvyBindingCheckAnchored(const char *expression)
{
#ifdef USE_PCRE_REGEX
	pcre2_code *regexp;
	int error;
	PCRE2_SIZE offset;
	uint32_t options = 0;
	int status;

	if (!expression)
		return IVY_EINVAL;
	regexp = pcre2_compile((PCRE2_SPTR)expression, PCRE2_ZERO_TERMINATED,
		PCRE_OPT, &error, &offset, NULL);
	if (!regexp)
		return error == PCRE2_ERROR_HEAP_FAILED ? IVY_ENOMEM : IVY_EINVAL;
	status = pcre2_pattern_info(regexp, PCRE2_INFO_ALLOPTIONS, &options);
	pcre2_code_free(regexp);
	if (status != 0)
		return IVY_EINVAL;
	return options & PCRE2_ANCHORED ? IVY_OK : IVY_EUNANCHORED;
#else
	(void)expression;
	/* Do not claim an anchoring guarantee this backend cannot establish. */
	return IVY_ESTATE;
#endif
}

IvyBinding IvyBindingCompile( const char * expression,  int *erroffset, const char **errmessage )
{
/*    static int called = 0;  */
/*    called ++;  */
/*    if ((called %1000) == 0) {  */
/*      printf ("DBG> IvyBindingCompile called =%d\n", called);  */
/*    }  */
	IvyBinding bind=0;
#ifdef USE_PCRE_REGEX
	pcre2_code *regexp;
	int errcode;
	PCRE2_SIZE pcre2_err_offset;

	regexp = pcre2_compile((PCRE2_SPTR)expression,
			       PCRE2_ZERO_TERMINATED,
			       PCRE_OPT,
			       &errcode,
			       &pcre2_err_offset,
			       NULL);
	if ( regexp != NULL )
		{
			bind = (IvyBinding)malloc( sizeof( struct _binding ));
			if ( ! bind ) 
			{
				perror( "IvyBindingCompile malloc error: ");
				exit(-1);
			}
			memset( bind, 0, sizeof(*bind ) );
			bind->regexp = regexp;
			pcre2_pattern_info(bind->regexp, PCRE2_INFO_CAPTURECOUNT, &bind->capture_count);
			/* JIT is optional; matching still works if it is unavailable. */
			(void)pcre2_jit_compile(bind->regexp, PCRE2_JIT_COMPLETE);
		}
		else
		{
		*erroffset = (int)pcre2_err_offset;
		if (pcre2_get_error_message(errcode, (PCRE2_UCHAR *)err_buf, sizeof(err_buf)) < 0)
			snprintf(err_buf, sizeof(err_buf), "PCRE2 error %d", errcode);
		*errmessage = err_buf;
		printf("Error compiling '%s', %s\n", expression, err_buf);
		}
#else  /* we don't USE_PCRE_REGEX */
	regex_t regexp;
	int reg;
	reg = regcomp(&regexp, expression, REGCOMP_OPT|REG_EXTENDED);
	if ( reg == 0 )
		{
			bind = (IvyBinding)malloc( sizeof( struct _binding ));
			if ( ! bind ) 
			{
				perror( "IvyBindingCompile malloc error: ");
				exit(-1);
			}
			memset( bind, 0, sizeof(*bind ) );
			bind->regexp = regexp;
		}
		else
		{
		regerror (reg, &regexp, err_buf, sizeof(err_buf) );
		*erroffset = 0;
		*errmessage = err_buf;
		printf("Error compiling '%s', %s\n", expression, err_buf);
		}
#endif /* USE_PCRE_REGEX */
	return bind;
}

void IvyBindingFree( IvyBinding bind )
{
/*   static int called = 0; */
/*   called ++; */
/*   if ((called %1000) == 0) { */
/*     printf ("DBG> IvyBindingFree called =%d\n", called); */
/*   } */
	if( bind == NULL ) return;
#ifdef USE_PCRE_REGEX
  pcre2_code_free(bind->regexp);
#else  /* we don't USE_PCRE_REGEX */
  regfree( &bind->regexp );
#endif /* USE_PCRE_REGEX */
  free ( bind );
}


int IvyBindingExec( IvyBinding bind, const char * message )
{
	int nb_match = 0;
	if( bind == NULL ) return nb_match;
#ifdef USE_PCRE_REGEX
	int match_rc;

	if (!IvyBindingPrepareMatchData(bind))
		return 0;

	match_rc = pcre2_match(bind->regexp,
			       (PCRE2_SPTR)message,
			       strlen(message),
			       0, /* debut */
			       0, /* no other regexp option */
			       thread_match_data,
			       NULL);
	if (match_rc == PCRE2_ERROR_NOMATCH)
		return 0;
	if (match_rc < 0)
		return 0;

	thread_ovector = pcre2_get_ovector_pointer(thread_match_data);
	thread_nb_match = match_rc;
	thread_last_bind = bind;
	nb_match = match_rc;
#else  /* we don't USE_PCRE_REGEX */
	{
		int index;
	memset( bind->match, -1, sizeof(bind->match )); /* work around bug !!!*/
	nb_match = regexec (&bind->regexp, message, MAX_MSG_FIELDS, bind->match, 0);
	if (nb_match == REG_NOMATCH)
		return 0;
	for (index = 1; index < MAX_MSG_FIELDS; index++ )
	{
		if ( bind->match[index].rm_so != -1 )
			nb_match++;
	}
	}
#endif /* USE_PCRE_REGEX */
	return nb_match;
}

void IvyBindingMatch( IvyBinding bind, const char *message, int argnum, int *arglen, const char **arg)
{
	if( bind == NULL ) return;
#ifdef USE_PCRE_REGEX
	if (thread_last_bind != bind || thread_ovector == NULL || argnum < 0 || argnum >= thread_nb_match)
		{
		*arglen = 0;
		*arg = NULL;
		return;
		}

	if (thread_ovector[2*argnum] == PCRE2_UNSET || thread_ovector[2*argnum+1] == PCRE2_UNSET)
		{
		*arglen = 0;
		*arg = NULL;
		return;
		}

	*arglen = (int)(thread_ovector[2*argnum+1]- thread_ovector[2*argnum]);
	*arg =   message + thread_ovector[2*argnum];
#else  /* we don't USE_PCRE_REGEX */
	
	regmatch_t* p;

	p = &bind->match[argnum];
	if ( p->rm_so != -1 ) {
			*arglen = p->rm_eo - p->rm_so;
			*arg = message + p->rm_so;
	} else { /* ARG VIDE */
			*arglen = 0;
			*arg = NULL;
	}
#endif /* USE_PCRE_REGEX */

}

/* This lexer matches the former ^\\^([a-zA-Z_0-9-]+) extraction regexp,
 * without a process-global compiled regexp or mutable regexp match storage. */
static int IvyFilterWordChar(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int IvyFilterValidWord(const char *word)
{
    if (!word || !*word) return 0;
    while (*word) {
        if (!IvyFilterWordChar((unsigned char)*word++)) return 0;
    }
    return 1;
}

void IvyFilterFree(IvyFilter filters)
{
    while (filters) {
        IvyFilter next = filters->next;
        free(filters->word);
        free(filters);
        filters = next;
    }
}

int IvyFilterContains(IvyFilter filters, const char *word)
{
    if (!word) return 0;
    for (; filters; filters = filters->next) {
        if (strcmp(filters->word, word) == 0) return 1;
    }
    return 0;
}

int IvyFilterAdd(IvyFilter *filters, const char *word)
{
    IvyFilter entry;
    if (!IvyFilterValidWord(word)) return IVY_EINVAL;
    if (IvyFilterContains(*filters, word)) return IVY_OK;
    entry = malloc(sizeof(*entry));
    if (!entry) return IVY_ENOMEM;
    entry->word = strdup(word);
    if (!entry->word) {
        free(entry);
        return IVY_ENOMEM;
    }
    entry->next = *filters;
    *filters = entry;
    return IVY_OK;
}

int IvyFilterCreate(int count, const char **words, IvyFilter *result)
{
    int i;
    *result = NULL;
    if (count < 0 || (count > 0 && !words)) return IVY_EINVAL;
    for (i = 0; i < count; ++i) {
        if (!IvyFilterValidWord(words[i])) return IVY_EINVAL;
    }
    for (i = 0; i < count; ++i) {
        int status = IvyFilterAdd(result, words[i]);
        if (status != IVY_OK) {
            IvyFilterFree(*result);
            *result = NULL;
            return status;
        }
    }
    return IVY_OK;
}

void IvyFilterRemove(IvyFilter *filters, const char *word)
{
    while (*filters) {
        IvyFilter entry = *filters;
        if (strcmp(entry->word, word) == 0) {
            *filters = entry->next;
            entry->next = NULL;
            IvyFilterFree(entry);
        } else {
            filters = &entry->next;
        }
    }
}

int IvyFilterAccepts(IvyFilter filters, const char *expression)
{
    const char *token;
    size_t length = 0;
    if (!expression) return 0;
    if (!filters || *expression != '^') return 1;
    token = expression + 1;
    while (IvyFilterWordChar((unsigned char)token[length])) ++length;
    if (!length) return 1;
    for (; filters; filters = filters->next) {
        /* Preserve prefix matching: ^TRA.* may match the declared class TRACK. */
        if (strncmp(filters->word, token, length) == 0) return 1;
    }
    return 0;
}

int IvyFilterCount(IvyFilter filters)
{
    int count = 0;
    for (; filters; filters = filters->next) {
        if (count == INT_MAX) return count;
        ++count;
    }
    return count;
}
