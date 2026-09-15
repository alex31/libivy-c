/*
 *	Ivy, C interface
 *
 *	Copyright (C) 1997-2006
 *	Centre d'Études de la Navigation Aérienne
 *
 *	Bind syntax for extracting message comtent 
 *  using regexp or other 
 *
 *	Authors: François-Régis Colin <fcolin@cena.fr>
 *
 *	$Id: ivybind.h 3475 2011-02-08 16:38:55Z fcolin $
 * 
 *	Please refer to file version.h for the
 *	copyright notice regarding this software
 */
/* Module de gestion de la syntaxe des messages Ivy */

typedef struct _binding *IvyBinding;

#ifdef __cplusplus
extern "C" {
#endif

/* Context-owned filters. The caller holds the owning context's bindings
 * lock when accessing a shared list. Detached lists can be built/freed unlocked. */
typedef struct _ivy_filter *IvyFilter;
int IvyFilterValidWord(const char *word);
int IvyFilterCreate(int count, const char **words, IvyFilter *result);
void IvyFilterFree(IvyFilter filters);
int IvyFilterAdd(IvyFilter *filters, const char *word);
void IvyFilterRemove(IvyFilter *filters, const char *word);
int IvyFilterContains(IvyFilter filters, const char *word);
int IvyFilterAccepts(IvyFilter filters, const char *expression);
int IvyFilterCount(IvyFilter filters);

/* Compatibility helpers operating on the current/default context. */
int IvyBindingGetFilterCount(void);
void IvyBindingSetFilter( int argc, const char ** argv );
void IvyBindingAddFilter( const char * argv );
void IvyBindingRemoveFilter( const char * arg );
int IvyBindingFilter( const char *expression );
void IvyBindindFilterCheck( const char *message );

/* Creation, Compilation */
IvyBinding IvyBindingCompile( const char *expression, int *erroffset, const char **errmessage );
void IvyBindingFree( IvyBinding _bind );

/* Internal check after Ivy interval expansion; returns an IvyStatus. */
int IvyBindingCheckAnchored(const char *expression);

/* Execution , extraction */
int IvyBindingExec( IvyBinding _bind, const char * message );
/* Get Argument */
void IvyBindingMatch( IvyBinding _bind, const char *message, int argnum, int *arglen, const char **arg );

/*
Liberation de memoire en fin d'execution 
*/
void IvyBindingTerminate(void);

#ifdef __cplusplus
}
#endif
