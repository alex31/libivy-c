#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "intervalRegexp.h"

#ifdef __PRETTY_FUNCTION__
#else
#define __PRETTY_FUNCTION__ __FUNCTION__
#endif


#define MAXINT( a , b ) ((a) > (b) ? (a) : (b))
#define MININT( a , b ) ((a) < (b) ? (a) : (b))

#define Perr(...) (perr ( __PRETTY_FUNCTION__, __VA_ARGS__))
#define CHECK_AND_RETURN(a)   if (strlen (locBuf) < buflen) {	\
			    strcpy (a, locBuf); \
			    return success; \
			 } else { \
                            return Perr ("CHECK_AND_RETURN"); }

#define EndLocBuf (&(locBuf[strlen(locBuf)]))
#ifdef WIN32
#define snprintf _snprintf
#define vsnprintf _vsnprintf
#endif
#define AddLocBuf(...) do {						\
    if (appendFormat (locBuf, sizeof (locBuf), __VA_ARGS__) == fail)	\
      return fail;							\
  } while (0)

typedef struct  {
  long max;
  int rank;
} NextMax ;


const bool success = 1;
const bool fail = 0;


static bool	strictPosRegexpGen (char *regexp, size_t buflen, long min, long max, 
				    const char* decimalPart, const char* boundDecimalPart);
static bool	genAtRank (char *regexp, size_t buflen, const char *min, const char *max, int rank);
static bool	genPreRank (char *preRank, size_t buflen, const char *min, const char *max, int rank);
static bool	genRank (char *outRank, size_t buflen, const char *min, const char *max, int rank);
static bool	genPostRank (char *postRank, size_t buflen, int rank);
static bool	substr (char *substring, size_t buflen, const char* expr, size_t pos, size_t len);
static char*	reverse (char *string);
static char*	longtoa (char *string, size_t buflen, long n);
static bool	nextMax (NextMax *out, const char *min, const char *max);
static bool	parseDecimalLong (const char *string, long *value);
static bool	appendFormat (char *buffer, size_t buflen, const char *fmt, ...);
static bool	perr (const char* func, const char *fmt, ...);



/*
#                                __ _                  _ __     _____
#                               / _` |                | '_ \   / ____|
#                 _ __    ___  | (_| |   ___  __  __  | |_) | | |  __    ___   _ __
#                | '__|  / _ \  \__, |  / _ \ \ \/ /  | .__/  | | |_ |  / _ \ | '_ \
#                | |    |  __/   __/ | |  __/  >  <   | |     | |__| | |  __/ | | | |
#                |_|     \___|  |___/   \___| /_/\_\  |_|      \_____|  \___| |_| |_|
*/
int regexpGen (char *regexp, size_t buflen, long min, long max, int flottant)
{
  const char *decimalPart = "";
  const char *boundDecimalPart = "";
  char locBuf [8192] = "(?:";

  
  if (flottant) {
    decimalPart = "(?:\\.\\d+)?";
    boundDecimalPart = "(?:\\.0+)?";
  }

  if (min > max) {
    long nmin = max;
    max = min;
    min = nmin;
  }

  if (min == max) {
    AddLocBuf ("%ld%s", min, decimalPart);
  } else if  (min < 0) {
    if (min == LONG_MIN) {
      return Perr ("min == LONG_MIN");
    }
    if  (max < 0) {
      /*      reg = '\-(?:' .  strictPosRegexpGen (-max, -min, decimalPart, boundDecimalPart). ')'; */
      AddLocBuf ("\\-(?:");
      if (strictPosRegexpGen (EndLocBuf, sizeof (locBuf)-strlen(locBuf), -max, -min, decimalPart,
			      boundDecimalPart) == fail) return fail;
      AddLocBuf (")");
    } else if  (max == 0) {
      AddLocBuf ("(?:0%s)|(?:-0%s)|-(?:", boundDecimalPart, decimalPart);
      if (strictPosRegexpGen (EndLocBuf, sizeof (locBuf)-strlen(locBuf), 1, -min, decimalPart, 
			  boundDecimalPart)== fail) return fail;
      AddLocBuf (")");
    } else {
      /*reg ='(?:' . regexpGen (min, 0,withDecimal) . '|' .  regexpGen (0, max, withDecimal). ')' ; */
      AddLocBuf ("(?:");
      if (regexpGen (EndLocBuf, sizeof (locBuf)-strlen(locBuf), min, 0, flottant)== fail) return fail;
      AddLocBuf ("|");
      if (regexpGen (EndLocBuf, sizeof (locBuf)-strlen(locBuf), 0, max, flottant)== fail) return fail;
      AddLocBuf (")");
    }
  } else if  (min == 0) {
    /* reg = "(?:0{decimalPart})|" . strictPosRegexpGen (1, max, decimalPart,boundDecimalPart) ; */
    AddLocBuf ("(?:0%s)|",decimalPart);
     if (strictPosRegexpGen (EndLocBuf, sizeof (locBuf)-strlen(locBuf), 1, max, decimalPart, 
			boundDecimalPart)== fail) return fail;
  } else {
     if (strictPosRegexpGen (EndLocBuf, sizeof (locBuf)-strlen(locBuf), min, max, decimalPart, 
			boundDecimalPart)== fail) return fail;
  }

  AddLocBuf (")(?![\\d.])");
  CHECK_AND_RETURN (regexp);
}

/*
#                        _             _           _      _____
#                       | |           (_)         | |    |  __ \
#                 ___   | |_    _ __   _     ___  | |_   | |__) |   ___    ___
#                / __|  | __|  | '__| | |   / __| | __|  |  ___/   / _ \  / __|
#                \__ \  \ |_   | |    | |  | (__  \ |_   | |      | (_) | \__ \
#                |___/   \__|  |_|    |_|   \___|  \__|  |_|       \___/  |___/
#                 _____            __ _                  _ __     _____
#                |  __ \          / _` |                | '_ \   / ____|
#                | |__) |   ___  | (_| |   ___  __  __  | |_) | | |  __    ___   _ __
#                |  _  /   / _ \  \__, |  / _ \ \ \/ /  | .__/  | | |_ |  / _ \ | '_ \
#                | | \ \  |  __/   __/ | |  __/  >  <   | |     | |__| | |  __/ | | | |
#                |_|  \_\  \___|  |___/   \___| /_/\_\  |_|      \_____|  \___| |_| |_|
*/
static bool strictPosRegexpGen (char *regexp, size_t buflen, long min, long max, const char* decimalPart, 
								   const char* boundDecimalPart)
{

#define maxSubReg 64
#define digitRegSize 128

  char		regList[maxSubReg][digitRegSize];
  char		locBuf[maxSubReg*digitRegSize] = "" ;
  size_t	regIndex = 0,i;
  char		maxAsString[32], minAsString[32];
  NextMax	nMax = {0,0};


  if ((min <= 0) || (max <= 0)) return Perr ("min or max <= 0");
  if (min == max) {
    AddLocBuf ("%ld", max);
  } else {
      
    max--;
  
    do {
      if (nextMax (&nMax,
		   longtoa (minAsString, sizeof (minAsString), min),
		   longtoa (maxAsString, sizeof (maxAsString), max)) == fail)
	return fail;
      if (genAtRank (regList[regIndex++], digitRegSize, minAsString, 
		     longtoa (maxAsString, sizeof (maxAsString), 
			   nMax.max), nMax.rank) == fail) return fail;
      if (regIndex == maxSubReg) return Perr ("regIndex == maxSubReg");
      min = nMax.max +1;
    } while (nMax.max != max);

    locBuf[0] = 0;
    for (i=0; i<regIndex; i++) {
      AddLocBuf ("(?:%s%s)|", regList[i], decimalPart);
    }
    
    if (locBuf[strlen(locBuf)-1] == '|') {
      locBuf[strlen(locBuf)-1] = 0;
    }
    max++;
    AddLocBuf ("|(?:%s%s)",
	       longtoa (maxAsString, sizeof (maxAsString), max),
	       boundDecimalPart);
  }

  CHECK_AND_RETURN (regexp);
}

/*
#                                        _      __  __
#                                       | |    |  \/  |
#                 _ __     ___  __  __  | |_   | \  / |   __ _  __  __
#                | '_ \   / _ \ \ \/ /  | __|  | |\/| |  / _` | \ \/ /
#                | | | | |  __/  >  <   \ |_   | |  | | | (_| |  >  <
#                |_| |_|  \___| /_/\_\   \__|  |_|  |_|  \__,_| /_/\_\
*/
static bool nextMax (NextMax *out, const char *min, const char *max)
{
  NextMax nextMaxi ={0,0};
  char revMin[32], revMax[32];
  size_t nbDigitsMin, nbDigitsMax;
  size_t rankRev=0, rankForw, rank=0;
  size_t i;
  long currMax;
  long parsedMin;

  nbDigitsMin = strlen (min);
  nbDigitsMax = strlen (max);

  if (out == NULL) return Perr ("out == NULL");
  if ((nbDigitsMin == 0) || (nbDigitsMax == 0)) return Perr ("empty bound");
  if (nbDigitsMin > nbDigitsMax) return Perr ("min has more digits than max");
  if (nbDigitsMax >= sizeof (revMin)) return Perr ("bound too large");
  if (parseDecimalLong (min, &parsedMin) == fail) return fail;
  if (parseDecimalLong (max, &currMax) == fail) return fail;
  if (parsedMin > currMax) return Perr ("min > max");

  for (i=0; i<nbDigitsMin; i++) {
    revMin[i]= min[nbDigitsMin-i-1];
    /*    printf ("DBG> nextMaxi  revMin[%d]= %c\n", nbDigitsMin-i-1, min[i]); */
  }
  for (i=nbDigitsMin; i<nbDigitsMax; i++) {
    revMin[i]= '0';
    /* printf ("DBG> nextMaxi  revMin[%d]= %c\n", nbDigitsMax-i, '0'); */
  }

  for (i=0; i<nbDigitsMax; i++) {
    revMax[i]= max[nbDigitsMax-i-1];
  }
  revMin[nbDigitsMax] = revMax[nbDigitsMax] = 0;
  rankForw = nbDigitsMax -1;

  /*  printf ("DBG> nextMaxi rev(%s)=%s rev(%s)=%s rankForw=%d\n", min, revMin, max, revMax, rankForw); */

  /*  en partant des unitées (digit de poids faible), premier digit de min != 0 */
  while ((rankRev < nbDigitsMax) && (revMin[rankRev] == '0')) rankRev++;
  if (rankRev == nbDigitsMax) return Perr ("min is zero");
  /*  en partant du digit de poids fort, premier digit de max != du même digit de revMin */
  while ((rankForw > 0) && (revMin[rankForw] == revMax[rankForw])) rankForw--;

  if (rankForw <= rankRev) {
    rank = rankForw;
    revMin[rankForw]= revMax[rankForw] - (rankForw ? 1 : 0);
    for (i=0; i<rankForw; i++) revMin[i] = '9';
  } else {
    rank =  rankRev; 
    for (i=0; i<=rankRev; i++) revMin[i] = '9';
  }

  if (parseDecimalLong (reverse (revMin), &nextMaxi.max) == fail) return fail;
  nextMaxi.rank = rank+1;
  
  if (nextMaxi.max > currMax) nextMaxi.max = currMax;

  /*  printf ("DBG> nextMaxi ('%s', '%s') = %d@%d\n", min, max, nextMaxi.max, nextMaxi.rank); */
  *out = nextMaxi;
  return (success);
}


/*
#                  __ _                    ____                           _
#                 / _` |                  / __ \                         | |
#                | (_| |   ___   _ __    / / _` |  _ __    __ _   _ __   | | _
#                 \__, |  / _ \ | '_ \  | | (_| | | '__|  / _` | | '_ \  | |/ /
#                  __/ | |  __/ | | | |  \ \__,_| | |    | (_| | | | | | |   <
#                 |___/   \___| |_| |_|   \____/  |_|     \__,_| |_| |_| |_|\_\
*/
static bool genAtRank (char *regexp, size_t buflen, const char *min, const char *max, int rank)
{
  char locBuf [512];

  if (genPreRank (locBuf, sizeof (locBuf), min, max, rank) == fail) return (fail);
  if (genRank (EndLocBuf, sizeof (locBuf)-strlen(locBuf), min, max, rank) == fail) return (fail);
  if (genPostRank (EndLocBuf, sizeof (locBuf)-strlen(locBuf), rank) == fail) return (fail);
  

  CHECK_AND_RETURN (regexp);
}

/*
#                  __ _                  _____                  _____                    _
#                 / _` |                |  __ \                |  __ \                  | |
#                | (_| |   ___   _ __   | |__) |  _ __    ___  | |__) |   __ _   _ __   | | _
#                 \__, |  / _ \ | '_ \  |  ___/  | '__|  / _ \ |  _  /   / _` | | '_ \  | |/ /
#                  __/ | |  __/ | | | | | |      | |    |  __/ | | \ \  | (_| | | | | | |   <
#                 |___/   \___| |_| |_| |_|      |_|     \___| |_|  \_\  \__,_| |_| |_| |_|\_\
*/
static bool genPreRank (char *preRank, size_t buflen, const char *min, const char *max, int rank)
{
  char locBuf [512], locBufMax[512];
  const char *lmin, *lmax;
  int i=0, j=0;

  while (min[i] == '0') i++;
  while (max[j] == '0') j++;

  lmin =  &(min[i]);
  lmax =  &(max[j]);
  
  /*  printf ("DBG> genPreRank (lmin='%s'[%d], lmax='%s'[%d], rank=%d\n", lmin, (int) strlen (lmin), lmax,   */
  /*  (int) strlen (lmax), rank); */

  if (substr (locBuf, sizeof (locBuf), lmin, 0, strlen (lmin) - rank) == fail) return fail;
  if (substr (locBufMax, sizeof (locBufMax), lmax, 0, strlen (lmax) - rank) == fail) return fail;

  if (strncmp (locBuf, locBufMax, MININT (sizeof (locBuf), sizeof (locBufMax))) != 0) 
    return Perr ("min=%s[%s] and max=%s[%s] should be invariants at rank %d", locBuf, min, locBufMax, max, rank);
  
  /*  printf ("DBG> genPreRank ('%s', '%s', %d) = '%s'\n", min, max, rank, locBuf); */

  CHECK_AND_RETURN (preRank);
}


/*
#                  __ _                  _____                    _
#                 / _` |                |  __ \                  | |
#                | (_| |   ___   _ __   | |__) |   __ _   _ __   | | _
#                 \__, |  / _ \ | '_ \  |  _  /   / _` | | '_ \  | |/ /
#                  __/ | |  __/ | | | | | | \ \  | (_| | | | | | |   <
#                 |___/   \___| |_| |_| |_|  \_\  \__,_| |_| |_| |_|\_\
*/
static bool genRank (char *outRank, size_t buflen, const char *min, const char *max, int rank)
{
  char locBuf [512];

  char a,b,lmin,lmax;
  a = min[strlen(min)-rank];
  b = max[strlen(max)-rank];

  lmin = MININT (a,b);
  lmax = MAXINT (a,b);
  
  if ((lmin == '0') && (lmax == '9')) {
    strcpy (locBuf, "\\d");
  } else if (lmin == lmax) {
    locBuf[0] = lmin;
    locBuf[1] = 0;
  } else if (lmax == (lmin+1)) {
    snprintf (locBuf, sizeof(locBuf), "[%c%c]", lmin, lmax);
  } else {
    snprintf (locBuf, sizeof(locBuf), "[%c-%c]", lmin, lmax);
  }

  CHECK_AND_RETURN (outRank);
}

/*
#                  __ _                  _____                   _      _____
#                 / _` |                |  __ \                 | |    |  __ \
#                | (_| |   ___   _ __   | |__) |   ___    ___   | |_   | |__) |   __ _   _ __
#                 \__, |  / _ \ | '_ \  |  ___/   / _ \  / __|  | __|  |  _  /   / _` | | '_ \
#                  __/ | |  __/ | | | | | |      | (_) | \__ \  \ |_   | | \ \  | (_| | | | | |
#                 |___/   \___| |_| |_| |_|       \___/  |___/   \__|  |_|  \_\  \__,_| |_| |_|
*/
static bool genPostRank (char *postRank, size_t buflen, int rank)
{
 char locBuf [512];

  if (rank <= 1) {
    strcpy (locBuf, "");
  } else if (rank == 2) {
    snprintf (locBuf, sizeof(locBuf), "\\d");
  } else {
    snprintf (locBuf, sizeof(locBuf), "\\d{%d}", rank -1);
  }

  CHECK_AND_RETURN (postRank);
}

/*
#                                _              _
#                               | |            | |
#                 ___    _   _  | |__    ___   | |_    _ __
#                / __|  | | | | | '_ \  / __|  | __|  | '__|
#                \__ \  | |_| | | |_) | \__ \  \ |_   | |
#                |___/   \__,_| |_.__/  |___/   \__|  |_|
*/
static bool substr (char *substring, size_t buflen, const char* expr, size_t pos, size_t len)
{
  char locBuf [512];
  size_t i, j=0;
  size_t exprLen;

  exprLen = strlen (expr);
  if (pos >= exprLen) {
    len = 0;
  } else if (len > exprLen - pos) {
    len = exprLen - pos;
  }
  if (len >= sizeof (locBuf)) return Perr ("substring too large");

  for (i=pos; i<(pos+len); i++) {
    locBuf[j++]= expr[i];
  }
  locBuf[j] = 0;

  /*  printf ("DBG> substr ('%s', %d, %d) = '%s'\n", expr, pos, len, locBuf); */
  CHECK_AND_RETURN (substring);
}

/*
#
#
#                 _ __    ___  __   __   ___   _ __   ___     ___
#                | '__|  / _ \ \ \ / /  / _ \ | '__| / __|   / _ \
#                | |    |  __/  \ V /  |  __/ | |    \__ \  |  __/
#                |_|     \___|   \_/    \___| |_|    |___/   \___|
*/
static char* reverse (char *string)
{
  size_t len = strlen (string);
  size_t i;

  for (i=0; i<(len/2); i++) {
    char tmp = string[i];
    string[i] = string[len-i-1];
    string[len-i-1] = tmp;
  }

  /*  printf ("DBG> reverse '%s' = '%s'\n", string, locBuf); */
  return (string);
}

static char* longtoa (char *string, size_t buflen, long n)
{
  snprintf (string, buflen, "%ld", n);
  return (string);
}

static bool appendFormat (char *buffer, size_t buflen, const char *fmt, ...)
{
  va_list args;
  size_t used;
  int written;

  if ((buffer == NULL) || (fmt == NULL) || (buflen == 0)) {
    return Perr ("invalid appendFormat argument");
  }

  used = strlen (buffer);
  if (used >= buflen) {
    return Perr ("appendFormat buffer already full");
  }

  va_start (args, fmt);
  written = vsnprintf (&buffer[used], buflen - used, fmt, args);
  va_end (args);

  if ((written < 0) || ((size_t) written >= buflen - used)) {
    buffer[buflen - 1] = '\0';
    return Perr ("appendFormat truncation");
  }

  return success;
}

static bool parseDecimalLong (const char *string, long *value)
{
  char *endptr;
  long parsed;
  size_t i;

  if ((string == NULL) || (string[0] == '\0') || (value == NULL)) {
    return Perr ("invalid parseDecimalLong argument");
  }

  for (i=0; string[i] != '\0'; i++) {
    if ((string[i] < '0') || (string[i] > '9')) {
      return Perr ("invalid decimal digit in '%s'", string);
    }
  }

  errno = 0;
  parsed = strtol (string, &endptr, 10);
  if ((errno == ERANGE) || (*endptr != '\0')) {
    return Perr ("invalid decimal value '%s'", string);
  }

  *value = parsed;
  return success;
}


/*
#                 _ __
#                | '_ \
#                | |_) |   ___   _ __   _ __
#                | .__/   / _ \ | '__| | '__|
#                | |     |  __/ | |    | |
#                |_|      \___| |_|    |_|
*/
static bool perr (const char* func, const char *fmt, ...)
{
  char buffer[2048];
  va_list args;
  va_start( args, fmt );     
  vsnprintf( buffer, sizeof(buffer), fmt, args );
  va_end( args );

  fprintf (stderr, "Erreur %s @ %s\n", buffer, func);
  return (fail);
}
