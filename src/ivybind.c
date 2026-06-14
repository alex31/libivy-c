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

static int err_offset;
static char err_buf[4096];

#ifdef USE_PCRE_REGEX
#if defined(_MSC_VER)
#define IVY_TLS __declspec(thread)
#elif defined(__GNUC__)
#define IVY_TLS __thread
#else
#define IVY_TLS _Thread_local
#endif
#endif /* USE_PCRE_REGEX */

struct _binding {
#ifdef USE_PCRE_REGEX
	pcre2_code *regexp;
	uint32_t capture_count;
#else  /* we don't USE_PCRE_REGEX */
	regex_t regexp;						/* la regexp sous forme machine */
	regmatch_t match[MAX_MSG_FIELDS+1];	/* resultat du match */
#endif /* USE_PCRE_REGEX */
	};

/* classes de messages emis par l'application utilise pour le filtrage */
typedef struct _filtred_word * FiltredWordPtr;
struct _filtred_word {                       /* requete d'emission d'un client */
        FiltredWordPtr next;
        const char *word;           /* entete de regexp a conserver */
};

static FiltredWordPtr messages_classes =0 ;
/* regexp d'extraction du mot clef des regexp client pour le filtrage des regexp , ca va c'est clair ??? */
static IvyBinding token_extract =0;

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
		err_offset = (int)pcre2_err_offset;
		*erroffset = err_offset;
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

/*filter Expression Bind  */

void IvyBindingSetFilter( int argc, const char **argv)
{
	int i;
	for ( i = 0 ; i < argc; i++ )
	{
	IvyBindingAddFilter( argv[i] );
	}

}

void IvyBindingAddFilter( const char *arg)
{
	const char *errbuf;
	int erroffset;
	if ( arg )
	{
	FiltredWordPtr word=0;
	IVY_LIST_ADD_START( messages_classes, word );
	word->word = strdup(arg);
  IVY_LIST_ADD_END( messages_classes, word );

	}
	/* compile the token extraction regexp */
	if ( !token_extract )
	{
		token_extract = IvyBindingCompile("^\\^([a-zA-Z_0-9-]+).*", & erroffset, & errbuf);
		if ( !token_extract )
		{
			printf("Error compiling Token Extract regexp: %s\n", errbuf);
		}
	}
}
void IvyBindingRemoveFilter( const char *arg)
{
	FiltredWordPtr word=0;
	FiltredWordPtr next=0;
	IVY_LIST_EACH_SAFE( messages_classes, word, next )
	{
		if ( strcmp( arg, word->word) == 0 )
			{
			free( (void*)word->word );
			IVY_LIST_REMOVE( messages_classes, word );
			}
	}
}
	
int IvyBindingFilter(const char *expression)
{
	FiltredWordPtr word=0;
	int err;
	int regexp_ok = 1; /* accepte tout par default */
	int tokenlen = 0;
	const char *token = NULL;
	
	if ( *expression =='^' && messages_classes !=0 )
	{
		regexp_ok = 0;
		
		/* extract token */
		err = IvyBindingExec( token_extract, expression );
		if ( err < 1 ) return 1;
		IvyBindingMatch( token_extract, expression , 1, &tokenlen, &token );
		if ( token == NULL || tokenlen <= 0 ) return 1;

		IVY_LIST_ITER( messages_classes, word, strncmp( word->word, token, (size_t)tokenlen ) != 0);

		if (word) {
		    return 1; 
		    }
		  /*		  else { */
		  /*printf ("DBG> %s eliminé [%s]\n", token, expression); */
		  /*} */
		
 	}
	return regexp_ok;
}
/* recherche si le message commence par un mot clef de la table */
void IvyBindindFilterCheck( const char *message )
{
	FiltredWordPtr word=0;
	IVY_LIST_ITER( messages_classes, word, strcmp( word->word, message ) != 0);

	if (word)
		{
		return; 
	}
	
	fprintf(stderr,"*** WARNING *** message '%s' not sent due to missing keyword in filter table!!!\n", message );    
}
void IvyBindingTerminate()
{
	FiltredWordPtr word=0;
	FiltredWordPtr next=0;
	
	IVY_LIST_EACH_SAFE( messages_classes, word, next )
  {
  free((void*) word->word );
  }
	IVY_LIST_EMPTY( messages_classes );
	messages_classes = 0;
}
