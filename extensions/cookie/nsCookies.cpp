/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: NPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is 
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Pierre Phaneuf <pp@ludusdesign.com>
 *   Henrik Gemal <mozilla@gemal.dk>
 *
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the NPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the NPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "nsCookies.h"
#include "nsPermissions.h"
#include "nsUtils.h"
#include "nsVoidArray.h"
#include "prprf.h"
#include "prmem.h"
#include "nsReadableUtils.h"
#include "nsIPref.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsIObserverService.h"
#include "nsICookieConsent.h"
#include "nsIURL.h"
#include "nsIHttpChannel.h"
#include "prnetdb.h"
#include "nsComObsolete.h"
#include <time.h>

#include "nsCRT.h"
#include "nsNetUtil.h"

// we want to explore making the document own the load group
// so we can associate the document URI with the load group.
// until this point, we have an evil hack:
#include "nsIHttpChannelInternal.h"  

#ifdef MOZ_LOGGING
// in order to do logging, the following environment variables need to be set:
//
//    set NSPR_LOG_MODULES=cookie:3 -- shows rejected cookies
//    set NSPR_LOG_MODULES=cookie:4 -- shows accepted and rejected cookies
//    set NSPR_LOG_FILE=c:\cookie.log
//
// this next define has to appear before the include of prolog.h
#define FORCE_PR_LOG /* Allow logging in the release build */
#include "prlog.h"
#endif

#if defined(PR_LOGGING)
PRLogModuleInfo* gCookieLog = nsnull;
#endif /* PR_LOGGING */

#define MAX_NUMBER_OF_COOKIES 300
#define MAX_COOKIES_PER_SERVER 20
#define MAX_BYTES_PER_COOKIE 4096  /* must be at least 1 */

#ifdef MOZ_PHOENIX
#define cookie_enabled "network.cookie.enable"
#define cookie_enabled_for_website_only "network.cookie.enableForOriginatingWebsiteOnly"
#define cookie_lifetimeBehaviorPref "network.cookie.enableForCurrentSessionOnly"
#else
#define cookie_disableCookieForMailNewsPref "network.cookie.disableCookieForMailNews"
#define cookie_lifetimePref "network.cookie.lifetimeOption"
#define cookie_lifetimeValue "network.cookie.lifetimeLimit"
#define cookie_behaviorPref "network.cookie.cookieBehavior"
#define cookie_lifetimeEnabledPref "network.cookie.lifetime.enabled"
#define cookie_lifetimeBehaviorPref "network.cookie.lifetime.behavior"
#define cookie_lifetimeDaysPref "network.cookie.lifetime.days"
#define cookie_p3pPref "network.cookie.p3p"
#endif
#define cookie_warningPref "network.cookie.warnAboutCookies"
#define cookie_strictDomainsPref "network.cookie.strictDomains"
// mac, windows, and unix use signed integers for time_t
#if defined(XP_MAC) || defined(XP_WIN) || defined(XP_UNIX)
#define MAX_EXPIRE (((unsigned) (~0) << 1) >> 1)
#else
#define MAX_EXPIRE 0
#endif

static const char kCookiesFileName[] = "cookies.txt";

MODULE_PRIVATE nsresult
cookie_ParseDate(const nsAFlatCString &date_string, time_t & date);

typedef enum {
  COOKIE_Normal,
  COOKIE_Discard,
  COOKIE_Trim,
  COOKIE_Ask
} COOKIE_LifetimeEnum;

PRIVATE PRBool cookie_changed = PR_FALSE;
PRIVATE PERMISSION_BehaviorEnum cookie_behavior = PERMISSION_Accept;
PRIVATE PRBool cookie_disableCookieForMailNews = PR_TRUE; //default -- disable is true
PRIVATE PRBool cookie_warning = PR_FALSE;
PRIVATE COOKIE_LifetimeEnum cookie_lifetimeOpt = COOKIE_Normal;
PRIVATE time_t cookie_lifetimeLimit = 90*24*60*60;
PRIVATE time_t cookie_lifetimeDays;
PRIVATE PRBool cookie_lifetimeCurrentSession;

PRIVATE char* cookie_P3P = nsnull;

/* cookie_P3P (above) consists of 8 characters having the following interpretation:
 *   [0]: behavior for first-party cookies when site has no privacy policy
 *   [1]: behavior for third-party cookies when site has no privacy policy
 *   [2]: behavior for first-party cookies when site uses PII with no user consent
 *   [3]: behavior for third-party cookies when site uses PII with no user consent
 *   [4]: behavior for first-party cookies when site uses PII with implicit consent only
 *   [5]: behavior for third-party cookies when site uses PII with implicit consent only
 *   [6]: behavior for first-party cookies when site uses PII with explicit consent
 *   [7]: behavior for third-party cookies when site uses PII with explicit consent
 *
 * note: PII = personally identifiable information
 *
 * each of the eight characters can be one of the following
 *   'a': accept the cookie
 *   'd': accept the cookie but downgrade it to a session cookie
 *   'r': reject the cookie
 *
 * The following defines are used to refer to these character positions and values
 */

#define P3P_UnknownPolicy   -1
#define P3P_NoPolicy         0
#define P3P_NoConsent        2
#define P3P_ImplicitConsent  4
#define P3P_ExplicitConsent  6
#define P3P_NoIdentInfo      8

#define P3P_Unknown    ' '
#define P3P_Accept     'a'
#define P3P_Downgrade  'd'
#define P3P_Reject     'r'
#define P3P_Flag       'f'

#define cookie_P3P_Default    "drdraaaa"

PRIVATE nsVoidArray * cookie_list=0;

static
time_t
get_current_time()
    /*
      We call this routine instead of |time()| because the latter returns
      different values on the Mac than on all other platforms (i.e., based on
      the Mac's 1900 epoch, vs all others 1970).  We can't call |PR_Now|
      directly, since the value is 64bits and too hard to manipulate.
      Hence, this cross-platform convenience routine.
    */
  {
    PRInt64 usecPerSec;
    LL_I2L(usecPerSec, 1000000L);

    PRTime now_useconds = PR_Now();

    PRInt64 now_seconds;
    LL_DIV(now_seconds, now_useconds, usecPerSec);

    time_t current_time_in_seconds;
    LL_L2I(current_time_in_seconds, now_seconds);

    return current_time_in_seconds;
  }

// Cookie logging macros for when PR_LOGGING is switched on
#define SET_COOKIE PR_TRUE
#define GET_COOKIE PR_FALSE

#ifdef PR_LOGGING
#define COOKIE_LOGFAILURE(a, b, c, d) cookie_LogFailure(a, b, c, d)
#define COOKIE_LOGSUCCESS(a, b, c, d) cookie_LogSuccess(a, b, c, d)

PRIVATE void
cookie_LogFailure(PRBool set_cookie, nsIURI * curURL, const char *cookieString, const char *reason) {
  if (!gCookieLog) {
    gCookieLog = PR_NewLogModule("cookie");
  }
  nsCAutoString spec;
  if (curURL)
    curURL->GetSpec(spec);

  PR_LOG(gCookieLog, PR_LOG_WARNING,
    ("%s%s%s\n", "===== ", set_cookie ? "COOKIE NOT ACCEPTED" : "COOKIE NOT SENT", " ====="));
  PR_LOG(gCookieLog, PR_LOG_WARNING,("request URL: %s\n", spec.get()));
  if (set_cookie) {
    PR_LOG(gCookieLog, PR_LOG_WARNING,("cookie string: %s\n", cookieString));
  }
  time_t curTime = get_current_time();
  PR_LOG(gCookieLog, PR_LOG_WARNING,("current time (gmt): %s", asctime(gmtime(&curTime))));
  PR_LOG(gCookieLog, PR_LOG_WARNING,("rejected because %s\n", reason));
  PR_LOG(gCookieLog, PR_LOG_WARNING,("\n"));
}

PRIVATE void
cookie_LogSuccess(PRBool set_cookie, nsIURI * curURL, const char *cookieString, cookie_CookieStruct * cookie) {
  if (!gCookieLog) {
    gCookieLog = PR_NewLogModule("cookie");
  }
  nsCAutoString spec;
  curURL->GetSpec(spec);

  PR_LOG(gCookieLog, PR_LOG_DEBUG,
    ("%s%s%s\n", "===== ", set_cookie ? "COOKIE ACCEPTED" : "COOKIE SENT", " ====="));
  PR_LOG(gCookieLog, PR_LOG_DEBUG,("request URL: %s\n", spec.get()));
  PR_LOG(gCookieLog, PR_LOG_DEBUG,("cookie string: %s\n", cookieString));
  time_t curTime = get_current_time();
  PR_LOG(gCookieLog, PR_LOG_DEBUG,("current time (gmt): %s", asctime(gmtime(&curTime))));

  if (set_cookie) {
    PR_LOG(gCookieLog, PR_LOG_DEBUG,("----------------\n"));
    PR_LOG(gCookieLog, PR_LOG_DEBUG,("name: %s\n", cookie->name.get()));
    PR_LOG(gCookieLog, PR_LOG_DEBUG,("value: %s\n", cookie->cookie.get()));
    PR_LOG(gCookieLog, PR_LOG_DEBUG,("%s: %s\n", cookie->isDomain ? "domain" : "host", cookie->host.get()));
    PR_LOG(gCookieLog, PR_LOG_DEBUG,("path: %s\n", cookie->path.get()));
    PR_LOG(gCookieLog, PR_LOG_DEBUG,("expires (gmt): %s",
           cookie->expires ? asctime(gmtime(&cookie->expires)) : "at end of session"));
    PR_LOG(gCookieLog, PR_LOG_DEBUG,("is secure: %s\n", cookie->isSecure ? "true" : "false"));
  }
  PR_LOG(gCookieLog, PR_LOG_DEBUG,("\n"));
}

// inline wrappers to make passing in nsAStrings easier
PRIVATE inline void
cookie_LogFailure(PRBool set_cookie, nsIURI * curURL, const nsAFlatCString &cookieString, const char *reason) {
  cookie_LogFailure(set_cookie, curURL, cookieString.get(), reason);
}

PRIVATE inline void
cookie_LogSuccess(PRBool set_cookie, nsIURI * curURL, const nsAFlatCString &cookieString, cookie_CookieStruct * cookie) {
  cookie_LogSuccess(set_cookie, curURL, cookieString.get(), cookie);
}
#else
#define COOKIE_LOGFAILURE(a, b, c, d) /* nothing */
#define COOKIE_LOGSUCCESS(a, b, c, d) /* nothing */
#endif

PR_STATIC_CALLBACK(PRBool) deleteCookie(void *aElement, void *aData) {
  cookie_CookieStruct *cookie = (cookie_CookieStruct*)aElement;
  delete cookie;
  return PR_TRUE;
}

PUBLIC void
COOKIE_RemoveAll()
{
  if (cookie_list) {
    cookie_list->EnumerateBackwards(deleteCookie, nsnull);
    cookie_changed = PR_TRUE;
    delete cookie_list;
    cookie_list = nsnull;
    if (cookie_P3P) {
      Recycle(cookie_P3P);
      cookie_P3P = nsnull;
    }
  }
}

PUBLIC void
COOKIE_DeletePersistentUserData(void)
{
  nsresult res;
  
  nsCOMPtr<nsIFile> cookiesFile;
  res = NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR, getter_AddRefs(cookiesFile));
  if (NS_SUCCEEDED(res)) {
    res = cookiesFile->AppendNative(NS_LITERAL_CSTRING(kCookiesFileName));
    if (NS_SUCCEEDED(res))
        (void) cookiesFile->Remove(PR_FALSE);
  }
}

PRIVATE void
cookie_RemoveOldestCookie(void) {
  cookie_CookieStruct * cookie_s;
  cookie_CookieStruct * oldest_cookie;

  if (cookie_list == nsnull) {
    return;
  }
   
  PRInt32 count = cookie_list->Count();
  if (count == 0) {
    return;
  }
  oldest_cookie = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(0));
  PRInt32 oldestLoc = 0;
  for (PRInt32 i = 1; i < count; ++i) {
    cookie_s = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie_s, "cookie list is corrupt");
    if(cookie_s->lastAccessed < oldest_cookie->lastAccessed) {
      oldest_cookie = cookie_s;
      oldestLoc = i;
    }
  }
  if(oldest_cookie) {
    cookie_list->RemoveElementAt(oldestLoc);
    delete oldest_cookie;
    cookie_changed = PR_TRUE;
  }

}

/* Remove any expired cookies from memory */
PRIVATE void
cookie_RemoveExpiredCookies() {
  cookie_CookieStruct * cookie_s;
  time_t cur_time = get_current_time();
  
  if (cookie_list == nsnull) {
    return;
  }
  
  for (PRInt32 i = cookie_list->Count(); i > 0;) {
    i--;
    cookie_s = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie_s, "corrupt cookie list");
      /* Don't get rid of expire time 0 because these need to last for 
       * the entire session. They'll get cleared on exit. */
      if( cookie_s->expires && (cookie_s->expires < cur_time) ) {
        cookie_list->RemoveElementAt(i);
        delete cookie_s;
        cookie_changed = PR_TRUE;
      }
  }
}

/* Remove any session cookies from memory */
PUBLIC void
COOKIE_RemoveSessionCookies() {
  cookie_CookieStruct * cookie_s;
  if (cookie_list == nsnull) {
    return;
  }
  
  for (PRInt32 i = cookie_list->Count(); i > 0;) {
    i--;
    cookie_s = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie_s, "corrupt cookie list");
      if(!cookie_s->expires) {
        cookie_list->RemoveElementAt(i);
        delete cookie_s;
      }
  }
}

/* checks to see if the maximum number of cookies per host
 * is being exceeded and deletes the oldest one in that
 * case
 */
PRIVATE void
cookie_CheckForMaxCookiesFromHost(const nsACString &cur_host) {
  cookie_CookieStruct * cookie_s;
  cookie_CookieStruct * oldest_cookie = 0;
  int cookie_count = 0;
  
  if (cookie_list == nsnull) {
    return;
  }
  
  PRInt32 count = cookie_list->Count();
  PRInt32 oldestLoc = -1;
  for (PRInt32 i = 0; i < count; ++i) {
    cookie_s = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie_s, "corrupt cookie list");
    if(cur_host.Equals(cookie_s->host, nsCaseInsensitiveCStringComparator())) {
      cookie_count++;
      if(!oldest_cookie || oldest_cookie->lastAccessed > cookie_s->lastAccessed) {
        oldest_cookie = cookie_s;
        oldestLoc = i;
      }
    }
  }
  if(cookie_count >= MAX_COOKIES_PER_SERVER && oldest_cookie) {
    NS_ASSERTION(oldestLoc > -1, "oldestLoc got out of sync with oldest_cookie");
    cookie_list->RemoveElementAt(oldestLoc);
    delete oldest_cookie;
    cookie_changed = PR_TRUE;
  }
}

/* search for previous exact match */
PRIVATE cookie_CookieStruct *
cookie_CheckForPrevCookie(const nsACString &path, const nsACString &hostname, const nsACString &name) {
  cookie_CookieStruct * cookie_s;
  if (!cookie_list) {
    return nsnull;
  }
  
  PRInt32 count = cookie_list->Count();
  for (PRInt32 i = 0; i < count; ++i) {
    cookie_s = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie_s, "corrupt cookie list");
    if(name.Equals(cookie_s->name) && path.Equals(cookie_s->path)
       && hostname.Equals(cookie_s->host, nsCaseInsensitiveCStringComparator())) {
      return(cookie_s);
    }
  }
  return(nsnull);
}

/* cookie utility functions */
PRIVATE void
cookie_SetBehaviorPref(PERMISSION_BehaviorEnum x, nsIPref* prefs) {
  cookie_behavior = x;
}

PRIVATE void
cookie_SetDisableCookieForMailNewsPref(PRBool x) {
  cookie_disableCookieForMailNews = x;
}

PRIVATE void
cookie_SetWarningPref(PRBool x) {
  cookie_warning = x;
}

PRIVATE void
cookie_SetLifetimePref(COOKIE_LifetimeEnum x) {
  cookie_lifetimeOpt = x;
}

PRIVATE void
cookie_SetLifetimeLimit(PRInt32 x) {
  // save limit as seconds instead of days
  cookie_lifetimeLimit = x*24*60*60;
}

PRIVATE PERMISSION_BehaviorEnum
cookie_GetBehaviorPref() {
  return cookie_behavior;
}

PRIVATE PRBool
cookie_GetDisableCookieForMailNewsPref() {
  return cookie_disableCookieForMailNews;
}

PRIVATE PRBool
cookie_GetWarningPref() {
  return cookie_warning;
}

PRIVATE COOKIE_LifetimeEnum
cookie_GetLifetimePref() {
  return cookie_lifetimeOpt;
}

PRIVATE time_t
cookie_GetLifetimeTime() {
  // return time after which lifetime is excessive
  return get_current_time() + cookie_lifetimeLimit;
}

#if 0
PRIVATE PRBool
cookie_GetLifetimeAsk(time_t expireTime) {
  // return true if we should ask about this cookie
  return (cookie_GetLifetimePref() == COOKIE_Ask)
    && (cookie_GetLifetimeTime() < expireTime);
}
#endif

PRIVATE time_t
cookie_TrimLifetime(time_t expires) {
  // return potentially-trimmed lifetime
  if (cookie_GetLifetimePref() == COOKIE_Trim) {
    // a limit of zero means expire cookies at end of session
    //    however we need to test for case of cookie being intentionally set to a time
    //    in the past (trick that servers use to delete cookies) and not turn that into
    //    a cookie that exires at end of current session.
    if ((cookie_lifetimeLimit == 0) &&
        ((unsigned)expires > (unsigned)get_current_time())) {
      return 0;
    }
    time_t limit = cookie_GetLifetimeTime();
    if ((unsigned)expires > (unsigned)limit) {
      return limit;
    }
  }
  return expires;
}

MODULE_PRIVATE int PR_CALLBACK
cookie_BehaviorPrefChanged(const char * newpref, void * data) {
  PRInt32 n;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
#ifdef MOZ_PHOENIX
  if (cookie_GetBehaviorPref() != PERMISSION_Accept && cookie_GetBehaviorPref() != PERMISSION_DontUse)
    return 0;
  PRBool cookiesEnabled;
  if (!prefs || NS_FAILED(prefs->GetBoolPref(cookie_enabled, &cookiesEnabled)) || cookiesEnabled) {
    n = PERMISSION_Accept;
  }
  else {
    n = PERMISSION_DontUse;
  }
  cookie_SetBehaviorPref((PERMISSION_BehaviorEnum)n, prefs);
#else
  if (!prefs || NS_FAILED(prefs->GetIntPref(cookie_behaviorPref, &n))) {
    n = PERMISSION_Accept;
  }
  cookie_SetBehaviorPref((PERMISSION_BehaviorEnum)n, prefs);
#endif    
  return 0;
}

#ifdef MOZ_PHOENIX
MODULE_PRIVATE int PR_CALLBACK
cookie_EnabledForOriginalOnlyPrefChanged(const char * newpref, void * data) {
  PRInt32 n;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  PRBool cookiesForWebsiteOnly = PR_FALSE;
  prefs->GetBoolPref(cookie_enabled_for_website_only, &cookiesForWebsiteOnly);
  if (cookiesForWebsiteOnly)
    n = PERMISSION_DontAcceptForeign;
  else if (cookie_GetBehaviorPref() != PERMISSION_DontUse)
    n = PERMISSION_Accept;
  cookie_SetBehaviorPref((PERMISSION_BehaviorEnum)n, prefs);
  return 0;
}
#endif

#ifndef MOZ_PHOENIX
MODULE_PRIVATE int PR_CALLBACK
cookie_DisableCookieForMailNewsPrefChanged(const char * newpref, void * data) {
  PRBool x;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (!prefs || NS_FAILED(prefs->GetBoolPref(cookie_disableCookieForMailNewsPref, &x))) {
    x = PR_TRUE;
  }
  cookie_SetDisableCookieForMailNewsPref(x);
  return 0;
}
#endif

MODULE_PRIVATE int PR_CALLBACK
cookie_WarningPrefChanged(const char * newpref, void * data) {
  PRBool x;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (!prefs || NS_FAILED(prefs->GetBoolPref(cookie_warningPref, &x))) {
    x = PR_FALSE;
  }
  cookie_SetWarningPref(x);
  return 0;
}

#ifndef MOZ_PHOENIX
MODULE_PRIVATE int PR_CALLBACK
cookie_LifetimeOptPrefChanged(const char * newpref, void * data) {
  PRInt32 n;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (!prefs || NS_FAILED(prefs->GetIntPref(cookie_lifetimePref, &n))) {
    n = COOKIE_Normal;
  }
  cookie_SetLifetimePref((COOKIE_LifetimeEnum)n);
  return 0;
}

MODULE_PRIVATE int PR_CALLBACK
cookie_LifetimeLimitPrefChanged(const char * newpref, void * data) {
  PRInt32 n;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (NS_SUCCEEDED(rv) && NS_SUCCEEDED(prefs->GetIntPref(cookie_lifetimeValue, &n))) {
    cookie_SetLifetimeLimit(n);
  }
  return 0;
}

MODULE_PRIVATE int PR_CALLBACK
cookie_LifetimeEnabledPrefChanged(const char * newpref, void * data) {
  PRInt32 n;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (!prefs || NS_FAILED(prefs->GetBoolPref(cookie_lifetimeEnabledPref, &n))) {
    n = PR_FALSE;
  }
  cookie_SetLifetimePref(n ? COOKIE_Trim : COOKIE_Normal);
  return 0;
}
#endif

MODULE_PRIVATE int PR_CALLBACK
cookie_LifetimeBehaviorPrefChanged(const char * newpref, void * data) {
nsresult rv;
nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
#ifdef MOZ_PHOENIX
  PRBool currentSessionOnly;
  prefs->GetBoolPref(cookie_lifetimeBehaviorPref, &currentSessionOnly);
  cookie_SetLifetimeLimit(currentSessionOnly ? 0 : cookie_lifetimeDays);
  cookie_SetLifetimePref(currentSessionOnly ? COOKIE_Trim : COOKIE_Normal);
  cookie_lifetimeCurrentSession = currentSessionOnly;
#else
  PRInt32 n;
  if (!prefs || NS_FAILED(prefs->GetIntPref(cookie_lifetimeBehaviorPref, &n))) {
    n = 0;
  }
  cookie_SetLifetimeLimit((n==0) ? 0 : cookie_lifetimeDays);
  cookie_lifetimeCurrentSession = (n==0);
#endif
  return 0;
}

#ifndef MOZ_PHOENIX
MODULE_PRIVATE int PR_CALLBACK
cookie_LifetimeDaysPrefChanged(const char * newpref, void * data) {
  PRInt32 n;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (!prefs || NS_SUCCEEDED(prefs->GetIntPref(cookie_lifetimeDaysPref, &n))) {
    cookie_lifetimeDays = n;
    if (!cookie_lifetimeCurrentSession) {
      cookie_SetLifetimeLimit(n);
    }
  }
  return 0;
}

MODULE_PRIVATE int PR_CALLBACK
cookie_P3PPrefChanged(const char * newpref, void * data) {
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (!prefs || NS_FAILED(prefs->CopyCharPref(cookie_p3pPref, &cookie_P3P))) {
    cookie_P3P = PL_strdup(cookie_P3P_Default);
  }
  return 0;
}
#endif

PRIVATE PRBool
cookie_SameDomain(const nsAFlatCString &currentHost, const nsAFlatCString &firstHost);

PUBLIC void
COOKIE_RegisterPrefCallbacks(void) {
  PRInt32 n;
  PRBool x;
  nsresult rv;
  nsCOMPtr<nsIPref> prefs(do_GetService(NS_PREF_CONTRACTID, &rv));
  if (!prefs) {
    return;
  }

#ifdef MOZ_PHOENIX
  // Initialize for cookie_behaviorPref
  if (NS_FAILED(prefs->GetBoolPref(cookie_enabled, &n))) {
    n = PERMISSION_Accept;
  }
  n = n ? PERMISSION_Accept : PERMISSION_DontUse;
  cookie_SetBehaviorPref((PERMISSION_BehaviorEnum)n, prefs);
  prefs->RegisterCallback(cookie_enabled, cookie_BehaviorPrefChanged, nsnull);
  
  if (NS_FAILED(prefs->GetBoolPref(cookie_enabled_for_website_only, &n))) {
    n = PERMISSION_Accept;
  }
  if (cookie_behavior != PERMISSION_DontUse)
    cookie_SetBehaviorPref((PERMISSION_BehaviorEnum)n, prefs);
  prefs->RegisterCallback(cookie_enabled_for_website_only, cookie_EnabledForOriginalOnlyPrefChanged, nsnull);
#else
  // Initialize for cookie_behaviorPref
  if (NS_FAILED(prefs->GetIntPref(cookie_behaviorPref, &n))) {
    n = PERMISSION_Accept;
  }
  cookie_SetBehaviorPref((PERMISSION_BehaviorEnum)n, prefs);
  prefs->RegisterCallback(cookie_behaviorPref, cookie_BehaviorPrefChanged, nsnull);

  // Initialize  cookie_disableCookieForMailNewsPref
  if (NS_FAILED(prefs->GetBoolPref(cookie_disableCookieForMailNewsPref, &x))) {
    x = PR_TRUE; //default --> disable is true
  }
  cookie_SetDisableCookieForMailNewsPref(x);
  prefs->RegisterCallback(cookie_disableCookieForMailNewsPref, cookie_DisableCookieForMailNewsPrefChanged, nsnull);

#endif

  // Initialize for cookie_warningPref
  if (NS_FAILED(prefs->GetBoolPref(cookie_warningPref, &x))) {
    x = PR_FALSE;
  }
  cookie_SetWarningPref(x);
  prefs->RegisterCallback(cookie_warningPref, cookie_WarningPrefChanged, nsnull);

  // Initialize for cookie_lifetime
  cookie_SetLifetimePref(COOKIE_Normal);
  cookie_lifetimeDays = 90;
  cookie_lifetimeCurrentSession = PR_FALSE;

#ifdef MOZ_PHOENIX
  PRBool forCurrentSession;
  if (NS_SUCCEEDED(prefs->GetBoolPref(cookie_lifetimeBehaviorPref, &forCurrentSession))) {
    cookie_lifetimeCurrentSession = forCurrentSession;
    cookie_SetLifetimeLimit(forCurrentSession ? 0 : cookie_lifetimeDays);
    cookie_SetLifetimePref(forCurrentSession ? COOKIE_Trim : COOKIE_Normal);
  }
#else
  if (NS_SUCCEEDED(prefs->GetIntPref(cookie_lifetimeDaysPref, &n))) {
    cookie_lifetimeDays = n;
  }
  if (NS_SUCCEEDED(prefs->GetIntPref(cookie_lifetimeBehaviorPref, &n))) {
    cookie_lifetimeCurrentSession = (n==0);
    cookie_SetLifetimeLimit((n==0) ? 0 : cookie_lifetimeDays);
  }
  if (NS_SUCCEEDED(prefs->GetBoolPref(cookie_lifetimeEnabledPref, &n))) {
    cookie_SetLifetimePref(n ? COOKIE_Trim : COOKIE_Normal);
  } 
  prefs->RegisterCallback(cookie_lifetimeEnabledPref, cookie_LifetimeEnabledPrefChanged, nsnull);
  prefs->RegisterCallback(cookie_lifetimeDaysPref, cookie_LifetimeDaysPrefChanged, nsnull);

  // Override cookie_lifetime initialization if the older prefs (with no UI) are used
  if (NS_SUCCEEDED(prefs->GetIntPref(cookie_lifetimePref, &n))) {
    cookie_SetLifetimePref((COOKIE_LifetimeEnum)n);
  }
  prefs->RegisterCallback(cookie_lifetimePref, cookie_LifetimeOptPrefChanged, nsnull);

  if (NS_SUCCEEDED(prefs->GetIntPref(cookie_lifetimeValue, &n))) {
    cookie_SetLifetimeLimit(n);
  }
  prefs->RegisterCallback(cookie_lifetimeValue, cookie_LifetimeLimitPrefChanged, nsnull);

  // Initialize for P3P prefs
  if (NS_FAILED(prefs->CopyCharPref(cookie_p3pPref, &cookie_P3P))) {
    cookie_P3P = PL_strdup(cookie_P3P_Default);
  }
  prefs->RegisterCallback(cookie_p3pPref, cookie_P3PPrefChanged, nsnull);
#endif
  prefs->RegisterCallback(cookie_lifetimeBehaviorPref, cookie_LifetimeBehaviorPrefChanged, nsnull);
}

static PRBool
cookie_IsIPAddress(const nsAFlatCString &name) {
  // determine if name is an IP address
  PRNetAddr addr;
  return PR_StringToNetAddr(name.get(), &addr) == PR_SUCCESS;
}

PRBool
cookie_IsInDomain(const nsAFlatCString &domain, const nsAFlatCString &host) {
  /* special case for domainName being identical to hostName
   *   This probably buys some efficiency.
   *   But, more important, it allows a site that has an IP address to set a domain
   *      cookie for that same domain.  This should be illegal (domain cookies for
   *      IP addresses make no sense) and will be trapped by the very next test.  However
   *      that test was actually preventing hotmail attachments from working.  See bug
   *      105917 for details.  So we will allow IP-address sites to set domain cookies in
   *      this one special case -- where the domain name is identically equal to the host
   *      name.
   */
  if (domain.Equals(host)) {
    return PR_TRUE;
  }

  /*
   * test for domain name being an IP address (e.g., 105.217.180.21) and reject if so
   */
  if (cookie_IsIPAddress(domain)) {
    return PR_FALSE;
  }

  /*
   * special case for domainName = .hostName
   *    e.g., hostName = netscape.com
   *          domainName = .netscape.com
   */
  if (domain.Equals(NS_LITERAL_CSTRING(".") + host, nsCaseInsensitiveCStringComparator())) {
    return PR_TRUE;
  }

  /*
   * normal case for hostName = x<domainName>
   *    e.g., hostName = home.netscape.com
   *          domainName = .netscape.com
   */
  /* this is the final test - if failed, not in domain */
  const nsACString &hostSubstring = Substring(host, host.Length() - domain.Length(), domain.Length());
  return domain.Equals(hostSubstring, nsCaseInsensitiveCStringComparator());
}

/* returns PR_TRUE if authorization is required
** 
**
** IMPORTANT:  Now that this routine is multi-threaded it is up
**             to the caller to free any returned string
*/
PUBLIC char *
COOKIE_GetCookie(nsIURI * address) {
  cookie_CookieStruct * cookie_s;
  PRBool isSecure = PR_TRUE;
  time_t cur_time = get_current_time();

  // initialize string for return data
  nsCAutoString cookieData;
  NS_NAMED_LITERAL_CSTRING(equals, "=");

  /* disable cookies if the user's prefs say so */
  if(cookie_GetBehaviorPref() == PERMISSION_DontUse) {
    COOKIE_LOGFAILURE(GET_COOKIE, address, "", "cookies are disabled");
    return nsnull;
  }

  /* Is this an https "secure" cookie? */
  if NS_FAILED(address->SchemeIs("https", &isSecure))
      isSecure = PR_TRUE;

  /* Don't let ftp sites read cookies (could be a security issue) */
  PRBool isFtp;
  if (NS_FAILED(address->SchemeIs("ftp", &isFtp)) || isFtp) {
    COOKIE_LOGFAILURE(GET_COOKIE, address, "", "ftp sites cannot read cookies");
    return nsnull;
  }
  /* search for all cookies */
  if (cookie_list == nsnull) {
    COOKIE_LOGFAILURE(GET_COOKIE, address, "", "cookie list is empty");
    return nsnull;
  }
  nsCAutoString host, path;
  // Get host and path
  nsresult result = address->GetHost(host);
  if (NS_FAILED(result)) {
    COOKIE_LOGFAILURE(GET_COOKIE, address, "", "GetHost failed");
    return nsnull;
  }
  if ((host.RFindChar(' ') != kNotFound) || (host.RFindChar('\t') != kNotFound)) {
    COOKIE_LOGFAILURE(GET_COOKIE, address, "", "host has embedded space character");
    return nsnull;
  }
  result = address->GetPath(path);
  if (NS_FAILED(result)) {
    COOKIE_LOGFAILURE(GET_COOKIE, address, "", "GetPath failed");
    return nsnull;
  }

  for (PRInt32 i = 0; i <cookie_list->Count(); i++) {
    cookie_s = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie_s, "corrupt cookie list");

    /* check the host or domain first */
    if(cookie_s->isDomain) {
      if(!cookie_IsInDomain(cookie_s->host, host)) {
        continue;
      }
    } else if(!host.Equals(cookie_s->host, nsCaseInsensitiveCStringComparator())) {
      /* hostname matchup failed. FAIL */
      continue;
    }

    /* shorter strings always come last so there can be no ambiquity */
    PRUint32 cookiePathLen = cookie_s->path.Length();
    if (cookiePathLen > 0 && cookie_s->path.Last() == '/') {
      cookiePathLen--;
    }

    if (Substring(path, 0, cookiePathLen).Equals(Substring(cookie_s->path, 0, cookiePathLen))) {
      char pathLastChar = path.CharAt(cookiePathLen);
      if (path.Length() > cookiePathLen &&
          pathLastChar != '/' && pathLastChar != '?' && pathLastChar != '#' && pathLastChar != ';') {
        /*
         * note that the '?' test above allows a site at host/abc?def to receive a cookie that
         * has a path attribute of abc.  This seems strange but at least one major site
         * (citibank, bug 156725) depends on it.  The test for # and ; are put in to proactively
         * avoid problems with other sites.
         */
        continue;
      }

      /* if the cookie is secure and the path isn't, dont send it */
      if (cookie_s->isSecure & !isSecure) {
        continue; /* back to top of while */
      }

      /* check for expired cookies */
      if( cookie_s->expires && (cookie_s->expires < cur_time) ) {
        /* expire and remove the cookie */
        cookie_list->RemoveElementAt(i--); /* decr i so next cookie isn't skipped */
        delete cookie_s;
        cookie_changed = PR_TRUE;
        continue;
      }

      /* if we've already added a cookie to the return list, append a "; " so
       * subsequent cookies are delimited in the final list. */
      if (!cookieData.IsEmpty()) cookieData.Append("; ");

      if (!cookie_s->name.IsEmpty()) {
        cookie_s->lastAccessed = cur_time;
        const nsACString &nameString = cookie_s->name + equals;

#ifdef PREVENT_DUPLICATE_NAMES
        /* make sure we don't have a previous name mapping already in the string */
        if (cookieData.Find(nameString) == kNotFound) {
          cookieData.Append(nameString);
          cookieData.Append(cookie_s->cookie);
        }
#else
        cookieData.Append(nameString);
        cookieData.Append(cookie_s->cookie);
#endif /* PREVENT_DUPLICATE_NAMES */
      } else {
        cookieData.Append(cookie_s->cookie);
      }
    }
  }

  /* may be nsnull */
  COOKIE_LOGSUCCESS(GET_COOKIE, address, cookieData, nsnull);
  return cookieData.IsEmpty() ? nsnull : ToNewCString(cookieData);
}

/* Determines whether the inlineHost is in the same domain as the currentHost.
 * For use with rfc 2109 compliance/non-compliance.
 */
PRIVATE PRBool
cookie_SameDomain(const nsAFlatCString &currentHost, const nsAFlatCString &firstHost) {
  /* case insensitive compare */
  if (currentHost.Equals(firstHost, nsCaseInsensitiveCStringComparator())) {
    return PR_TRUE;
  }

  /* check for at least two dots before continuing, if there are
   * not two dots we don't have enough information to determine
   * whether or not the currentHost is within the firstHost
   */
  PRInt32 currentHostDot = currentHost.FindChar('.');
  PRInt32 firstHostDot = firstHost.FindChar('.');
  if (currentHostDot == kNotFound || firstHostDot == kNotFound) {
    return PR_FALSE;
  }
  PRInt32 dot = firstHost.FindChar('.', firstHostDot + 1);
  /* |dot + 1 == length| handles .com. case */
  if (dot == kNotFound || dot + 1 == (PRInt32) firstHost.Length()) {
    return PR_FALSE;
  }

  // we use PL_strcasecmp here, because to do this with string fu is an ugly overkill
  return !PL_strcasecmp(currentHost.get() + currentHostDot, firstHost.get() + firstHostDot);
}

PRBool
cookie_isFromMailNews(nsIURI *firstURL) {
  
  if (!firstURL) 
    return PR_FALSE;

  nsCAutoString schemeString;
  nsresult rv = firstURL->GetScheme(schemeString);
  if (NS_FAILED(rv))  //malformed uri
    return PR_FALSE; 
  
  return (schemeString.Equals(NS_LITERAL_CSTRING("imap")) || 
          schemeString.Equals(NS_LITERAL_CSTRING("news")) ||
          schemeString.Equals(NS_LITERAL_CSTRING("snews")) ||
          schemeString.Equals(NS_LITERAL_CSTRING("mailbox")));
}


PRBool
cookie_isForeign (nsIURI * curURL, nsIURI * firstURL) {
  if (!firstURL) {
    return PR_FALSE;
  }
  PRBool isChrome = PR_FALSE;
  nsresult rv = firstURL->SchemeIs("chrome", &isChrome);
  if (NS_SUCCEEDED(rv) && isChrome) {
     return PR_FALSE; // chrome URLs are never foreign (otherwise sidebar cookies won't work)
  }
  nsCAutoString curHost, firstHost;

  // Get hosts
  rv = curURL->GetHost(curHost);
  if (NS_FAILED(rv)) {
    return PR_FALSE;
  }

  rv = firstURL->GetHost(firstHost);
  if (NS_FAILED(rv)) {
    return PR_FALSE;
  }

  /* determine if it's foreign */
  return !cookie_SameDomain(curHost, firstHost);
}

nsCookieStatus
cookie_GetStatus(char decision) {
  switch (decision) {
    case ' ':
      return nsICookie::STATUS_UNKNOWN;
    case 'a':
      return nsICookie::STATUS_ACCEPTED;
    case 'd':
      return nsICookie::STATUS_DOWNGRADED;
    case 'f':
      return nsICookie::STATUS_FLAGGED;
    case 'r':
      return nsICookie::STATUS_REJECTED;
  }
  return nsICookie::STATUS_UNKNOWN;
}

nsCookiePolicy
cookie_GetPolicy(int policy) {
  switch (policy) {
    case P3P_NoPolicy:
      return nsICookie::POLICY_NONE;
    case P3P_NoConsent:
      return nsICookie::POLICY_NO_CONSENT;
    case P3P_ImplicitConsent:
      return nsICookie::POLICY_IMPLICIT_CONSENT;
    case P3P_ExplicitConsent:
      return nsICookie::POLICY_EXPLICIT_CONSENT;
    case P3P_NoIdentInfo:
      return nsICookie::POLICY_NO_II;
  }
  return nsICookie::POLICY_UNKNOWN;
}

/*
 * returns P3P_NoPolicy, P3P_NoConsent, P3P_ImplicitConsent,
 * P3P_ExplicitConsent, or P3P_NoIdentInfo based on site
 */
int
P3P_SitePolicy(nsIURI * curURL, nsIHttpChannel* aHttpChannel) {
  int consent = P3P_UnknownPolicy;
  if (cookie_GetBehaviorPref() == PERMISSION_P3P) {
    nsCOMPtr<nsICookieConsent> p3p(do_GetService(NS_COOKIECONSENT_CONTRACTID));
    if (p3p) {
      nsCAutoString curURLSpec;
      if (NS_FAILED(curURL->GetSpec(curURLSpec)))
          return consent;
      p3p->GetConsent(curURLSpec.get(),aHttpChannel,&consent);
    }
  }
  return consent;
}

/*
 * returns P3P_Accept, P3P_Downgrade, P3P_Flag, or P3P_Reject based on user's preferences
 */
int
cookie_P3PUserPref(PRInt32 policy, PRBool foreign) {
  NS_ASSERTION(policy == P3P_UnknownPolicy ||
               policy == P3P_NoPolicy ||
               policy == P3P_NoConsent ||
               policy == P3P_ImplicitConsent ||
               policy == P3P_ExplicitConsent ||
               policy == P3P_NoIdentInfo,
               "invalid value for p3p policy");
  if (policy == P3P_NoIdentInfo) {
    /* if site does not collect identifiable info, then treat it as if it did and
     * asked for explicit consent */
    policy = P3P_ExplicitConsent;
  }
  // note: P3P_UnknownPolicy means that the p3p module was not installed
  if (cookie_P3P && PL_strlen(cookie_P3P) == 8 && policy != P3P_UnknownPolicy) {
    return (foreign ? cookie_P3P[policy+1] : cookie_P3P[policy]);
  } else {
    return P3P_Accept;
  }
}

/*
 * returns STATUS_ACCEPT, STATUS_DOWNGRADE, STATUS_FLAG, or STATUS_REJECT based on user's preferences
 */
nsCookieStatus
cookie_P3PDecision (nsIURI * curURL, nsIURI * firstURL, nsIHttpChannel* aHttpChannel) {
  return cookie_GetStatus(
           cookie_P3PUserPref(
             P3P_SitePolicy(curURL, aHttpChannel),
             cookie_isForeign(curURL, firstURL)));
}

/* returns PR_TRUE if authorization is required
** 
**
** IMPORTANT:  Now that this routine is multi-threaded it is up
**             to the caller to free any returned string
*/
PUBLIC char *
COOKIE_GetCookieFromHttp(nsIURI * address, nsIURI * firstAddress) {
  if ((cookie_GetBehaviorPref() == PERMISSION_DontAcceptForeign) &&
      (!firstAddress || cookie_isForeign(address, firstAddress))) {

    /*
     * WARNING!!! This is a different behavior than 4.x.  In 4.x we used this pref to
     * control the setting of cookies only.  Here we are also blocking the getting of
     * cookies if the pref is set.  It may be that we need a separate pref to block the
     * getting of cookies.  But for now we are putting both under one pref since that
     * is cleaner.  If it turns out that this breaks some important websites, we may
     * have to resort to two prefs
     */

    COOKIE_LOGFAILURE(GET_COOKIE, address, "", "Originating server test failed");
    return nsnull;
  }
  return COOKIE_GetCookie(address);
}

MODULE_PRIVATE PRBool
cookie_IsFromHost(cookie_CookieStruct *cookie_s, const nsAFlatCString &host) {
  if (!cookie_s) {
    return PR_FALSE;
  }
  if (cookie_s->isDomain) {
    /* compare the tail end of host to cook_s->host */
    return cookie_IsInDomain(cookie_s->host, host);
  } else {
    return host.Equals(cookie_s->host, nsCaseInsensitiveCStringComparator());
  }
}

/* find out how many cookies this host has already set */
PRIVATE PRInt32
cookie_Count(const nsAFlatCString &host) {
  PRInt32 count = 0;
  cookie_CookieStruct * cookie;
  if (!cookie_list || host.IsEmpty()) return 0;

  for (PRInt32 i = cookie_list->Count(); i > 0;) {
    i--;
    cookie = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie, "corrupt cookie list");
    if (cookie_IsFromHost(cookie, host)) count++;
  }
  return count;
}

/* Java script is calling COOKIE_SetCookieString, netlib is calling 
 * this via COOKIE_SetCookieStringFromHttp.
 */
PRIVATE void
cookie_SetCookieString(nsIURI *curURL, nsIPrompt *aPrompter, const char *setCookieHeader,
                       cookie_CookieStruct *aCookie, time_t timeToExpire, nsIHttpChannel *aHttpChannel,
                       nsCookieStatus status)
{
  // reject cookie if it's over the size limit, per RFC2109
  if ((aCookie->name.Length() + aCookie->cookie.Length()) > MAX_BYTES_PER_COOKIE) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "cookie too big (> 4kb)");
    return;
  }

  nsCAutoString cur_host, cur_path;
  nsresult rv;
  rv = curURL->GetHost(cur_host);
  if (NS_FAILED(rv)) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "GetHost failed");
    return;
  }

  /* Don't let ftp sites set cookies (could be a security issue) */
  PRBool isFtp;
  if (NS_FAILED(curURL->SchemeIs("ftp", &isFtp)) || isFtp) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "ftp sites cannot set cookies");
    return;
  }

  rv = curURL->GetPath(cur_path);
  if (NS_FAILED(rv)) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "GetPath failed");
    return;
  }

  PRBool pref_scd = PR_FALSE;

  if(cookie_GetBehaviorPref() == PERMISSION_DontUse) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "cookies are disabled");
    return;
  }

  if(cookie_GetLifetimePref() == COOKIE_Discard) {
    if(cookie_GetLifetimeTime() < timeToExpire) {
      COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "cookie lifetime test failed");
      return;
    }
  }

  // process domain attribute
  if (!aCookie->host.IsEmpty()) {
    if (aCookie->host.First() != '.' && !cookie_IsIPAddress(cur_host)) {
      // if host is not an IP address, force domain name to start with a dot
      aCookie->host.Insert(NS_LITERAL_CSTRING("."), 0);
    }

    /*
     * verify that this host has the authority to set for this domain.   We do
     * this by making sure that the host is in the domain.  We also require
     * that a domain have at least two periods to prevent domains of the form
     * ".com" and ".edu"
     *
     * Also make sure that there is more stuff after the second dot to prevent ".com."
     */
    PRInt32 dot = aCookie->host.FindChar('.');
    dot = aCookie->host.FindChar('.', ++dot);
    if (dot == kNotFound || ++dot == (PRInt32) aCookie->host.Length()) {
      /* did not pass two dot test. FAIL */
      COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "failed the two-dot test");
      return;
    }

    /* check to see if the host is in the domain */
    if (!cookie_IsInDomain(aCookie->host, cur_host)) {
      COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "host is not in the domain");
      return;
    }

    /*
     * check that portion of host not in domain does not contain a dot
     *    This satisfies the fourth requirement in section 4.3.2 of the cookie
     *    spec rfc 2109 (see www.cis.ohio-state.edu/htbin/rfc/rfc2109.html).
     *    It prevents a host of the form x.y.co.nz from setting cookies in the
     *    entire .co.nz domain.  Note that this doesn't really solve the problem,
     *    it justs makes it more unlikely.  Sites such as y.co.nz can still set
     *    cookies for the entire .co.nz domain.
     *
     *  Although this is the right thing to do(tm), it breaks too many sites.  
     *  So only do it if the restrictCookieDomains pref is PR_TRUE.
     *
     */
    nsresult rv;
    nsCOMPtr<nsIPref> prefs = do_GetService(NS_PREF_CONTRACTID, &rv);
    if (NS_FAILED(rv) || !prefs || NS_FAILED(prefs->GetBoolPref(cookie_strictDomainsPref, &pref_scd))) {
      pref_scd = PR_FALSE;
    }
    if ( pref_scd == PR_TRUE ) {
      dot = cur_host.FindChar('.', 0, cur_host.Length() - aCookie->host.Length());
      if (dot != kNotFound) {
        COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "host minus domain failed the no-dot test");
        return;
      }
    }

    /* all tests passed, set isDomain flag */
    aCookie->isDomain = PR_TRUE;

  } else {
    aCookie->host = cur_host;
    aCookie->isDomain = PR_FALSE;
  }

  /* set path if none found in header, else verify that host has authority for indicated path */
  if (aCookie->path.IsEmpty()) {
    /* Strip down everything after the last slash to get the path,
     * ignoring slashes in the query string part.
     */
    nsCOMPtr<nsIURL> url = do_QueryInterface(curURL);
    if (url) {
      url->GetDirectory(aCookie->path);
    }
#if 0
  } else {
    /*
     * The following test is part of the RFC2109 spec.  Loosely speaking, it says that a site
     * cannot set a cookie for a path that it is not on.  See bug 155083.  However this patch
     * broke several sites -- nordea (bug 155768) and citibank (bug 156725).  So this test is being
     * bracketed by an if statement to allow it to be disabled in the event that we cannot
     * evangelize these sites.
     */
    if (cur_path.Find(path_from_header, PR_FALSE, 0, aCookie->path.Length())) {
      COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "failed the path test");
      return;
    }
#endif
  }

  // put the remaining cookie information into the struct.
  aCookie->expires = cookie_TrimLifetime(timeToExpire);
  aCookie->lastAccessed = get_current_time();
  aCookie->status = status;
  aCookie->policy = cookie_GetPolicy(P3P_SitePolicy(curURL, aHttpChannel));

  // check permissions from site permission list, or ask the user,
  // to determine if we can set the cookie
  PRBool permission = PR_TRUE;
  if (NS_SUCCEEDED(PERMISSION_Read())) {
    // get the number of previous cookies from host, and whether the same cookie
    // has been previously set; to pass to the permission checker
    PRInt32 count = cookie_Count(aCookie->host);
    PRBool modify = cookie_CheckForPrevCookie(aCookie->path, aCookie->host, aCookie->name) != nsnull;

    // put the cookie information into the cookie structure.
    nsCOMPtr<nsICookie> thisCookie = 
      new nsCookie(aCookie->name,
                   aCookie->cookie,
                   aCookie->isDomain,
                   aCookie->host,
                   aCookie->path,
                   aCookie->isSecure,
                   cookie_TrimLifetime(timeToExpire),
                   status,
                   cookie_GetPolicy(P3P_SitePolicy(curURL, aHttpChannel)));

    // We need to think about adding logic to ask the user about cookies that have
    // excessive lifetimes, but it shouldn't be done until generalized per-site preferences
    // are available. cookie_GetLifetimeAsk() is part of this.
    permission = Permission_Check(aPrompter, aCookie->host.get(), COOKIEPERMISSION,
                                  cookie_GetWarningPref(), // || cookie_GetLifetimeAsk(timeToExpire)
                                  thisCookie, count, modify);
  }
  if (!permission) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "cookies blocked for this site");
    return;
  }

  rv = COOKIE_AddCookie(aCookie->host, aCookie->path,
                        aCookie->name, aCookie->cookie,
                        aCookie->isSecure, aCookie->isDomain, aCookie->expires,
                        aCookie->status, aCookie->policy);
  if (NS_FAILED(rv)) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "couldn't add cookie to list");
  } else {
    COOKIE_LOGSUCCESS(SET_COOKIE, curURL, setCookieHeader, aCookie);
  }
}

PUBLIC void
COOKIE_SetCookieString(nsIURI * aURL, nsIPrompt *aPrompter, const char * setCookieHeader, nsIHttpChannel* aHttpChannel) {
  nsCOMPtr<nsIURI> pFirstURL;
  nsresult rv;

  if (aHttpChannel) {
    nsCOMPtr<nsIHttpChannelInternal> httpInternal = do_QueryInterface(aHttpChannel);
    if (!httpInternal) {
      COOKIE_LOGFAILURE(SET_COOKIE, aURL, setCookieHeader, "unable to QueryInterface httpInternal");
      return;
    }
    rv = httpInternal->GetDocumentURI(getter_AddRefs(pFirstURL));
    if (NS_FAILED(rv)) {
      COOKIE_LOGFAILURE(SET_COOKIE, aURL, setCookieHeader, "unable to determine first URL");
      return;
    }
  }
  COOKIE_SetCookieStringFromHttp(aURL, pFirstURL, aPrompter, setCookieHeader, 0, aHttpChannel);
}

// The following comment block elucidates the function of cookie_ParseAttributes.

/******************************************************************************
 ** Augmented BNF, modified from RFC2109 Section 4.2.2 and RFC2616 Section 2.1
 ** please note: this BNF deviates from both specifications, and reflects this
 ** implementation. <bnf> indicates a reference to the defined grammer "bnf".

 ** Differences from RFC2109/2616 and explanations:
    1. implied *LWS
         The grammar described by this specification is word-based. Except
         where noted otherwise, linear white space (<LWS>) can be included
         between any two adjacent words (token or quoted-string), and
         between adjacent words and separators, without changing the
         interpretation of a field.
       <LWS> according to spec is SP|HT|CR|LF, but here, we allow only SP | HT.

    2. We use CR | LF as cookie separators, not ',' per spec, since ',' is in
       common use inside values.

    3. tokens and values have looser restrictions on allowed characters than
       spec. This is also due to certain characters being in common use inside
       values. We allow only '=' to separate token/value pairs, and ';' to
       terminate tokens or values.

    4. where appropriate, full <OCTET>s are allowed, where the spec dictates to
       reject control chars or non-ASCII chars. This is erring on the loose
       side, since there's probably no good reason to enforce this strictness.

    5. cookie <VALUE> is optional, where spec requires it. This is a fairly
       trivial case, but allows the flexibility of setting only a cookie <NAME>.

 ** Begin BNF:
    token         = 1*<any allowed-chars except separators>
    value         = token-value | quoted-string
    token-value   = 1*<any allowed-chars except value-sep>
    quoted-string = ( <"> *( qdtext | quoted-pair ) <"> )
    qdtext        = <any OCTET except <"> or NUL>        ; CR | LF allowed here
    quoted-pair   = "\" <any OCTET except NUL>           ; CR | LF allowed here
    separators    = ";" | "=" | LWS
    value-sep     = ";"
    cookie-sep    = CR | LF
    allowed-chars = <any OCTET except NUL or cookie-sep>
    OCTET         = <any 8-bit sequence of data>
    LWS           = SP | HT
    NUL           = <US-ASCII NUL, null control character (0)>
    CR            = <US-ASCII CR, carriage return (13)>
    LF            = <US-ASCII LF, linefeed (10)>
    SP            = <US-ASCII SP, space (32)>
    HT            = <US-ASCII HT, horizontal-tab (9)>

    set-cookie    = "Set-Cookie:" cookies
    cookies       = cookie *( cookie-sep cookie )
    cookie        = NAME ["=" VALUE] *(";" cookie-av)    ; cookie NAME/VALUE must come first
    NAME          = token                                ; cookie name
    VALUE         = value                                ; cookie value
    cookie-av     = token ["=" value]

    valid values for cookie-av (checked post-parsing) are:
    cookie-av     = "Path" "=" value
                  | "Domain"  "=" value
                  | "Expires" "=" value
                  | "Max-Age" "=" value
                  | "Comment" "=" value
                  | "Version" "=" value
                  | "Secure"

******************************************************************************/

// helper functions for cookie_GetTokenValue
PRIVATE inline PRBool iswhitespace    (char c) { return c == ' '  || c == '\t'; }
PRIVATE inline PRBool isterminator    (char c) { return c == '\n' || c == '\r'; }
PRIVATE inline PRBool isvalueseparator(char c) { return isterminator(c) || c == ';'; }
PRIVATE inline PRBool istokenseparator(char c) { return isvalueseparator(c) || iswhitespace(c) || c == '='; }

// Parse a single token/value pair.
// Returns PR_TRUE if a cookie terminator is found, so caller can parse new cookie.
PRIVATE PRBool
cookie_GetTokenValue(nsASingleFragmentCString::const_char_iterator &aIter,
                     nsASingleFragmentCString::const_char_iterator &aEndIter,
                     nsDependentSingleFragmentCSubstring &aTokenString,
                     nsDependentSingleFragmentCSubstring &aTokenValue)
{
  nsASingleFragmentCString::const_char_iterator start;
  // initialize value string to clear garbage
  aTokenValue.Rebind(aIter, aIter);

  // find <token>
  while (aIter != aEndIter && iswhitespace(*aIter))
    ++aIter;
  start = aIter;
  while (aIter != aEndIter && !istokenseparator(*aIter))
    ++aIter;
  aTokenString.Rebind(start, aIter);

  // now expire whitespace to see if '=' awaits us
  while (aIter != aEndIter && iswhitespace(*aIter)) // skip over spaces at end of cookie name
    ++aIter;

  if (*aIter == '=') {
    // find <value>
    while (++aIter != aEndIter && iswhitespace(*aIter));

    start = aIter;

    if (*aIter == '"') {
      // process <quoted-string>
      // (note: cookie terminators, CR | LF, allowed)
      // assume value mangled if no terminating '"', return
      while (++aIter != aEndIter && *aIter != '"') {
        // if <qdtext> (backwhacked char), skip over it. this allows '\"' in <quoted-string>.
        // we increment once over the backwhack, nullcheck, then continue to the 'while',
        // which increments over the backwhacked char.
        if (*aIter == '\\' && ++aIter == aEndIter)
          break;
      }

      if (aIter != aEndIter) {
        // include terminating quote in attribute string
        aTokenValue.Rebind(start, ++aIter);
        // skip to next ';'
        while (aIter != aEndIter && !isvalueseparator(*aIter))
          ++aIter;
      }
    } else {
      // process <token-value>
      // just look for ';' to terminate ('=' allowed)
      while (aIter != aEndIter && !isvalueseparator(*aIter))
        ++aIter;

      // remove trailing <LWS>; first check we're not at the beginning
      if (aIter != start) {
        nsASingleFragmentCString::const_char_iterator lastSpace = aIter;
        while (--lastSpace != start && iswhitespace(*lastSpace));
        aTokenValue.Rebind(start, ++lastSpace);
      }
    }
  }

  // aIter is on ';', or terminator, or EOS
  if (aIter != aEndIter) {
    // if on terminator, increment past & return PR_TRUE to process new cookie
    if (isterminator(*aIter)) {
      ++aIter;
      return PR_TRUE;
    }
    // fall-through: aIter is on ';', increment and return PR_FALSE
    ++aIter;
  }
  return PR_FALSE;
}

// Parses attributes from cookie header. expires/max-age attributes aren't folded into the
// cookie struct here, because we don't know which one to use until we've parsed the header.
PRIVATE PRBool
cookie_ParseAttributes(nsDependentCString  &aCookieHeader,
                       cookie_CookieStruct *aCookie,
                       nsACString          &aExpiresAttribute,
                       nsACString          &aMaxageAttribute)
{
  static NS_NAMED_LITERAL_CSTRING(kPath,    "path"   );
  static NS_NAMED_LITERAL_CSTRING(kDomain,  "domain" );
  static NS_NAMED_LITERAL_CSTRING(kExpires, "expires");
  static NS_NAMED_LITERAL_CSTRING(kMaxage,  "max-age");
  static NS_NAMED_LITERAL_CSTRING(kSecure,  "secure" );

  nsASingleFragmentCString::const_char_iterator tempBegin, tempEnd;
  nsASingleFragmentCString::const_char_iterator cookieStart, cookieEnd;
  aCookieHeader.BeginReading(cookieStart);
  aCookieHeader.EndReading(cookieEnd);

  aCookie->isSecure = PR_FALSE;

  nsDependentSingleFragmentCSubstring tokenString(cookieStart, cookieStart);
  nsDependentSingleFragmentCSubstring tokenValue (cookieStart, cookieStart);
  PRBool newCookie;

  // extract cookie NAME & VALUE (first attribute), and copy the strings
  // if we find multiple cookies, return for processing
  // note: if there's no '=', we assume token is NAME, not VALUE.
  //       the old code assumed VALUE instead.
  newCookie = cookie_GetTokenValue(cookieStart, cookieEnd, tokenString, tokenValue);
  aCookie->name = tokenString;
  aCookie->cookie = tokenValue;

  // extract remaining attributes
  while (cookieStart != cookieEnd && !newCookie) {
    newCookie = cookie_GetTokenValue(cookieStart, cookieEnd, tokenString, tokenValue);

    if (!tokenValue.IsEmpty() && *tokenValue.BeginReading(tempBegin) == '"'
                              && *tokenValue.EndReading(tempEnd) == '"') {
      // our parameter is a quoted-string; remove quotes for later parsing
      tokenValue.Rebind(++tempBegin, --tempEnd);
    }

    // decide which attribute we have, and copy the string
    if (tokenString.Equals(kPath, nsCaseInsensitiveCStringComparator()))
      aCookie->path = tokenValue;

    else if (tokenString.Equals(kDomain, nsCaseInsensitiveCStringComparator()))
      aCookie->host = tokenValue;

    else if (tokenString.Equals(kExpires, nsCaseInsensitiveCStringComparator()))
      aExpiresAttribute = tokenValue;

    else if (tokenString.Equals(kMaxage, nsCaseInsensitiveCStringComparator()))
      aMaxageAttribute = tokenValue;

    // ignore any tokenValue for isSecure; just set the boolean
    else if (tokenString.Equals(kSecure, nsCaseInsensitiveCStringComparator()))
      aCookie->isSecure = PR_TRUE;
  }

  // rebind aCookieHeader, in case we need to process another cookie
  aCookieHeader.Rebind(cookieStart, cookieEnd);
  return newCookie;
}

/* This function wrapper wraps COOKIE_SetCookieString for the purposes of 
 * determining whether or not a cookie is inline (we need the URL struct, 
 * and outputFormat to do so).  this is called from NET_ParseMimeHeaders 
 * in mkhttp.c
 * This routine does not need to worry about the cookie lock since all of
 *   the work is handled by sub-routines
*/

PUBLIC void
COOKIE_SetCookieStringFromHttp(nsIURI * curURL, nsIURI * firstURL, nsIPrompt *aPrompter,  const char * setCookieHeader, char * server_date, nsIHttpChannel* aHttpChannel) {
  // switch to a nice string type now
  nsDependentCString cookieHeader(setCookieHeader);

  /* If the outputFormat is not PRESENT (the url is not going to the screen), and not
   *  SAVE AS (shift-click) then 
   *  the cookie being set is defined as inline so we need to do what the user wants us
   *  to based on his preference to deal with foreign cookies. If it's not inline, just set
   *  the cookie.
   */
  time_t gmtCookieExpires=0, expires=0, sDate;

  /* check to see if P3P pref is satisfied */
  nsCookieStatus status = nsICookie::STATUS_UNKNOWN;
  if (cookie_GetBehaviorPref() == PERMISSION_P3P) {
    status = cookie_P3PDecision(curURL, firstURL, aHttpChannel);
    if (status == nsICookie::STATUS_REJECTED) {
      nsCOMPtr<nsIObserverService> os(do_GetService("@mozilla.org/observer-service;1"));
      if (os)
        os->NotifyObservers(nsnull, "cookieIcon", NS_LITERAL_STRING("on").get());
      COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "P3P test failed");
      return;
    }
  }
 
  /* check for foreign cookie if pref says to reject such */
  if ((cookie_GetBehaviorPref() == PERMISSION_DontAcceptForeign) &&
      cookie_isForeign(curURL, firstURL)) {
    /* it's a foreign cookie so don't set the cookie */
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "originating server test failed");
    return;
  }

  /* check if a Mail/News message is setting the cookie */
  if (cookie_GetDisableCookieForMailNewsPref() && cookie_isFromMailNews(firstURL)) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "cookies disabled for mailnews");
    return;
  }

  // initialize structs and strings, and parse attributes from cookie header.
  // cookieAttributes stores all the attributes parsed from the cookie;
  // expires and maxage are separate, because we have to process them to find the expiry.
  nsCAutoString expiresAttribute, maxageAttribute;
  cookie_CookieStruct *cookieAttributes = new cookie_CookieStruct;
  if (!cookieAttributes) {
    COOKIE_LOGFAILURE(SET_COOKIE, curURL, setCookieHeader, "unable to allocate memory for new cookie");
    delete cookieAttributes;
    return;
  }

  // newCookie says whether there are multiple cookies in the header; so we can handle them separately.
  // we don't need the cookieHeader string for processing this cookie anymore;
  // so this function uses it as an outparam to point to the next cookie, if there is one.
  PRBool newCookie = cookie_ParseAttributes(cookieHeader, cookieAttributes, expiresAttribute, maxageAttribute);

  /* Determine when the cookie should expire. This is done by taking the difference between 
   * the server time and the time the server wants the cookie to expire, and adding that 
   * difference to the client time. This localizes the client time regardless of whether or
   * not the TZ environment variable was set on the client.
   */

  // check for max-age attribute first; this overrides expires attribute
  if (!maxageAttribute.IsEmpty()) {
    PRInt32 errorCode;
    time_t delta = maxageAttribute.ToInteger(&errorCode, 10); // obtain numeric value of argument
    if (delta <= 0) {  // negative max-age is not allowed; but this is more robust
      gmtCookieExpires = 1; // force cookie to expire immediately
    } else {
      gmtCookieExpires = get_current_time() + delta;
    }

  // check for expires attribute
  } else if (!expiresAttribute.IsEmpty()) {
    if (NS_SUCCEEDED(cookie_ParseDate(expiresAttribute, expires)) &&
        expires == 0) {
      expires = 1; // force immediate expiry
    }

    if (server_date && *server_date) {
      cookie_ParseDate(nsDependentCString(server_date), sDate);
    } else {
      sDate = get_current_time();
    }

    if (sDate && expires) {
      if (expires < sDate) {
        gmtCookieExpires = 1; // force immediate expiry
      } else {
        gmtCookieExpires = expires - sDate + get_current_time();
        // if overflow 
        if( gmtCookieExpires < get_current_time()) {
          gmtCookieExpires = MAX_EXPIRE; // max int
        }
      }
    }
  }

  // call the main cookie processing function
  cookie_SetCookieString(curURL, aPrompter, setCookieHeader, cookieAttributes,
                         gmtCookieExpires, aHttpChannel, status);

  // we're finished with attributes - data has been copied, if required
  delete cookieAttributes;

  // finally, if we have multiple cookies to process, recurse.
  if (newCookie) {
    COOKIE_SetCookieStringFromHttp(curURL, firstURL, aPrompter, cookieHeader.get(),
                                   server_date, aHttpChannel);
  }
}

/* saves out the HTTP cookies to disk */
PUBLIC nsresult
COOKIE_Write() {
  if (!cookie_changed) {
    return NS_OK;
  }
  cookie_CookieStruct * cookie_s;
  time_t cur_date = get_current_time();
  char date_string[36];

  nsCOMPtr<nsIFile> dirSpec;

  nsresult rv;
  rv = CKutil_ProfileDirectory(getter_AddRefs(dirSpec));
  if (NS_FAILED(rv)) {
    return rv;
  }

  dirSpec->AppendNative(NS_LITERAL_CSTRING(kCookiesFileName));

  nsCOMPtr<nsIOutputStream> strm;
  rv = NS_NewLocalFileOutputStream(getter_AddRefs(strm),
                                   dirSpec);
  if (NS_FAILED(rv)) {
    NS_ERROR("failed to open cookies.txt for writing");
    return rv;
  }

#define COOKIEFILE_LINE1 "# HTTP Cookie File\n"
#define COOKIEFILE_LINE2 "# http://www.netscape.com/newsref/std/cookie_spec.html\n"
#define COOKIEFILE_LINE3 "# This is a generated file!  Do not edit.\n"
#define COOKIEFILE_LINE4 "# To delete cookies, use the Cookie Manager.\n\n"

  PRUint32 ignore;
  strm->Write(COOKIEFILE_LINE1, sizeof(COOKIEFILE_LINE1)-1, &ignore);
  strm->Write(COOKIEFILE_LINE2, sizeof(COOKIEFILE_LINE2)-1, &ignore);
  strm->Write(COOKIEFILE_LINE3, sizeof(COOKIEFILE_LINE3)-1, &ignore);
  strm->Write(COOKIEFILE_LINE4, sizeof(COOKIEFILE_LINE4)-1, &ignore);

  /* format shall be:
   *
   * host \t isDomain \t path \t secure \t expires \t name \t cookie
   *
   * isDomain is PR_TRUE or PR_FALSE
   * secure is PR_TRUE or PR_FALSE
   * expires is a time_t integer
   * cookie can have tabs
   */
  PRInt32 count = cookie_list ? cookie_list->Count() : 0;
  for (PRInt32 i = 0; i < count; ++i) {
    cookie_s = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
    NS_ASSERTION(cookie_s, "corrupt cookie list");
     if (cookie_s->expires < cur_date || cookie_s->status == nsICookie::STATUS_DOWNGRADED) {
       /* don't write entry if cookie has expired, has no expiratio date, or has been downgraded */
        continue;
      }

      strm->Write(cookie_s->host.get(), cookie_s->host.Length(), &ignore);

      if (cookie_s->isDomain) {
        strm->Write("\tTRUE\t", 6, &ignore);
      } else {
        strm->Write("\tFALSE\t", 7, &ignore);
      }

      strm->Write(cookie_s->path.get(), cookie_s->path.Length(), &ignore);

      if (cookie_s->isSecure) {
        strm->Write("\tTRUE\t", 6, &ignore);
      } else {
        strm->Write("\tFALSE\t", 7, &ignore);
      }

      PR_snprintf(date_string, sizeof(date_string), "%lu", NS_STATIC_CAST(unsigned long, cookie_s->expires));

      strm->Write(date_string, strlen(date_string), &ignore);
      strm->Write("\t", 1, &ignore);
      strm->Write(cookie_s->name.get(), cookie_s->name.Length(), &ignore);
      strm->Write("\t", 1, &ignore);
      strm->Write(cookie_s->cookie.get(), cookie_s->cookie.Length(), &ignore);
      strm->Write("\n", 1, &ignore);
  }

  cookie_changed = PR_FALSE;
  return NS_OK;
}

/* Notify cookie manager dialog to update its display */
PUBLIC nsresult
COOKIE_Notify() {
  nsCOMPtr<nsIObserverService> os(do_GetService("@mozilla.org/observer-service;1"));
  if (os) {
    os->NotifyObservers(nsnull, "cookieChanged", NS_LITERAL_STRING("cookies").get());
  }
  return NS_OK;
}

/* reads HTTP cookies from disk */
PUBLIC nsresult
COOKIE_Read() {
  if (cookie_list) {
    return NS_OK;
  }
  cookie_CookieStruct *new_cookie, *tmp_cookie_ptr;
  nsCAutoString buffer;
  PRBool added_to_list;

  PRBool exists = PR_FALSE;
  nsCOMPtr<nsIFile> dirSpec;
  nsresult rv = CKutil_ProfileDirectory(getter_AddRefs(dirSpec));
  if (NS_FAILED(rv)) {
    return rv;
  }
  dirSpec->AppendNative(NS_LITERAL_CSTRING(kCookiesFileName));
  if (NS_FAILED(dirSpec->Exists(&exists)) || !exists) {
    /* file doesn't exist -- that's not an error */
    return NS_OK;
  }

  nsCOMPtr<nsIInputStream> strm;
  rv = NS_NewLocalFileInputStream(getter_AddRefs(strm),
                                  dirSpec);
  if (NS_FAILED(rv)) {
    return rv;
  }

  /* format is:
   *
   * host \t isDomain \t path \t secure \t expires \t name \t cookie
   *
   * if this format isn't respected we move onto the next line in the file.
   * isDomain is PR_TRUE or PR_FALSE -- defaulting to PR_FALSE
   * secure is PR_TRUE or PR_FALSE   -- should default to PR_TRUE
   * expires is a time_t integer
   * cookie can have tabs
   */

#define BUFSIZE 4096
  char readbuffer[BUFSIZE];
  PRInt32 next = BUFSIZE, count = BUFSIZE;
  while (CKutil_GetLine(strm, readbuffer, BUFSIZE, next, count, buffer) != -1){
    added_to_list = PR_FALSE;

    if (!buffer.IsEmpty()) {
      char bufferFirstChar = buffer.First();
      if (bufferFirstChar == '#' || bufferFirstChar == '\n' || bufferFirstChar == '\r') {
        continue;
      }
    }
    int hostIndex, isDomainIndex, pathIndex, secureIndex, expiresIndex, nameIndex, cookieIndex;
    hostIndex = 0;
    if ((isDomainIndex=buffer.FindChar('\t', hostIndex)+1) == 0 ||
        (pathIndex=buffer.FindChar('\t', isDomainIndex)+1) == 0 ||
        (secureIndex=buffer.FindChar('\t', pathIndex)+1) == 0 ||
        (expiresIndex=buffer.FindChar('\t', secureIndex)+1) == 0 ||
        (nameIndex=buffer.FindChar('\t', expiresIndex)+1) == 0 ||
        (cookieIndex=buffer.FindChar('\t', nameIndex)+1) == 0 ) {
      continue;
    }
    nsCAutoString host, isDomain, path, isSecure, expires, name, cookie;
    buffer.Mid(host, hostIndex, isDomainIndex-hostIndex-1);
    buffer.Mid(isDomain, isDomainIndex, pathIndex-isDomainIndex-1);
    buffer.Mid(path, pathIndex, secureIndex-pathIndex-1);
    buffer.Mid(isSecure, secureIndex, expiresIndex-secureIndex-1);
    buffer.Mid(expires, expiresIndex, nameIndex-expiresIndex-1);
    buffer.Mid(name, nameIndex, cookieIndex-nameIndex-1);
    buffer.Mid(cookie, cookieIndex, buffer.Length()-cookieIndex);

    /* create a new cookie_struct and fill it in */
    new_cookie = new cookie_CookieStruct;
    if (!new_cookie) {
      return NS_ERROR_OUT_OF_MEMORY;
    }
    new_cookie->name = name;
    new_cookie->cookie = cookie;
    new_cookie->host = host;
    new_cookie->path = path;
    if (isDomain.Equals(NS_LITERAL_CSTRING("TRUE"))) {
      new_cookie->isDomain = PR_TRUE;
    } else {
      new_cookie->isDomain = PR_FALSE;
    }
    if (isSecure.Equals(NS_LITERAL_CSTRING("TRUE"))) {
      new_cookie->isSecure = PR_TRUE;
    } else {
      new_cookie->isSecure = PR_FALSE;
    }
    PRInt32 errorCode;
    new_cookie->expires = expires.ToInteger(&errorCode, 10);

    new_cookie->status = nsICookie::STATUS_UNKNOWN;
    new_cookie->policy = nsICookie::POLICY_UNKNOWN;

    /* check for bad legacy cookies (domain not starting with a dot) */
    if (new_cookie->isDomain && !new_cookie->host.IsEmpty() && new_cookie->host.First() != '.') {
      /* bad cookie, discard it */
      delete new_cookie;
      continue;
    }

    /* check for bad legacy cookies (domain containing a port) */
    if (new_cookie->host.FindChar(':') != kNotFound) {
      /* bad cookie, discard it */
      delete new_cookie;
      continue;
    }
  
    /* start new cookie list if one does not already exist */
    if (!cookie_list) {
      cookie_list = new nsVoidArray();
      if (!cookie_list) {
        deleteCookie((void*)new_cookie, nsnull);
        return NS_ERROR_OUT_OF_MEMORY;
      }
    }

    /* add new cookie to the list so that it is before any strings of smaller length */
    for (PRInt32 i = cookie_list->Count(); i > 0;) {
      i--;
      tmp_cookie_ptr = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
      NS_ASSERTION(tmp_cookie_ptr, "corrupt cookie list");
      if (new_cookie->path.Length() <= tmp_cookie_ptr->path.Length()) {
        cookie_list->InsertElementAt(new_cookie, i);
        added_to_list = PR_TRUE;
        break;
      }
    }

    /* no shorter strings found in list so add new cookie at end */
    if (!added_to_list) {
      cookie_list->AppendElement(new_cookie);
    }
  }

  cookie_changed = PR_FALSE;
  return NS_OK;
}

PUBLIC PRInt32
COOKIE_Count() {
  if (!cookie_list) {
    return 0;
  }
  /* Get rid of any expired cookies now so user doesn't
   * think/see that we're keeping cookies in memory.
   */
  cookie_RemoveExpiredCookies();

  return cookie_list->Count();
}

/*
 * return a string that has each " of the argument string
 * replaced with \" so it can be used inside a quoted string
 */
PRIVATE void
cookie_FixQuoted(const nsACString &inStr, nsACString &outStr) {
  outStr = inStr;

  PRInt32 pos = 0;
  while (pos < (PRInt32) outStr.Length() && (pos = outStr.FindChar('"', pos)) != kNotFound) {
    outStr.Insert('\\', pos);
    ++pos;
    ++pos;
   }
 }

PUBLIC nsresult
COOKIE_Enumerate (PRInt32 count, nsACString &name, nsACString &value, PRBool &isDomain,
                  nsACString &host, nsACString &path, PRBool &isSecure, PRUint64 &expires,
                  nsCookieStatus &status, nsCookiePolicy &policy)
{
  if (!cookie_list) {
    return NS_ERROR_FAILURE;
  }
  cookie_CookieStruct *cookie;
  if (count < 0 || count >= cookie_list->Count()) {
    NS_ERROR("bad cookie index");
    return NS_ERROR_UNEXPECTED;
  }
  cookie = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(count));
  NS_ASSERTION(cookie, "corrupt cookie list");

  cookie_FixQuoted(cookie->name, name);
  cookie_FixQuoted(cookie->cookie, value);
  cookie_FixQuoted(cookie->host, host);
  cookie_FixQuoted(cookie->path, path);
  isDomain = cookie->isDomain;
  isSecure = cookie->isSecure;
  // *expires = cookie->expires; -- no good on mac, using next line instead
  LL_UI2L(expires, cookie->expires);
  status = cookie->status;
  policy = cookie->policy;

  return NS_OK;
}

PUBLIC void
COOKIE_Remove
    (const nsACString &host, const nsACString &name, const nsACString &path, const PRBool blocked) {
  cookie_CookieStruct * cookie;
  PRInt32 count = 0;

  /* step through all cookies searching for indicated one */
  if (cookie_list) {
    count = cookie_list->Count();
    while (count>0) {
      count--;
      cookie = NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(count));
      NS_ASSERTION(cookie, "corrupt cookie list");
      if (cookie->host.Equals(host) &&
          cookie->name.Equals(name) &&
          cookie->path.Equals(path)) {
        if (blocked) {
          PRInt32 pos = 0;
          while (cookie->host.CharAt(pos) == '.') {
            ++pos;
          }
          if (NS_SUCCEEDED(PERMISSION_Read())) {
            const nsACString &hostSubstring = Substring(cookie->host, pos, cookie->host.Length() - pos);
            Permission_AddHost(PromiseFlatCString(hostSubstring), PR_FALSE, COOKIEPERMISSION, PR_TRUE);
          }
        }
        cookie_list->RemoveElementAt(count);
        delete cookie;
        cookie_changed = PR_TRUE;
        COOKIE_Write();
        break;
      }
    }
  }
}

// this is a backdoor. normally use COOKIE_SetCookieString.
// caller does NOT hand off ownership of strings to us.
PUBLIC nsresult
COOKIE_AddCookie(const nsACString &aDomain, const nsACString &aPath,
                 const nsACString &aName, const nsACString &aValue,
                 PRBool aSecure, PRBool aIsDomain,
                 time_t aExpires,
                 nsCookieStatus aStatus, nsCookiePolicy aPolicy)
{
  PRBool cookieAdded = PR_FALSE;
  cookie_CookieStruct *prev_cookie;

  /* limit the number of cookies */
  cookie_CheckForMaxCookiesFromHost(aDomain);
  if (cookie_list && cookie_list->Count() >= MAX_NUMBER_OF_COOKIES)
    cookie_RemoveOldestCookie();

  aExpires = cookie_TrimLifetime(aExpires);

  prev_cookie = cookie_CheckForPrevCookie (aPath, aDomain, aName);
  if(prev_cookie) {
    prev_cookie->path = aPath;
    prev_cookie->host = aDomain;
    prev_cookie->name = aName;
    prev_cookie->cookie = aValue;
    prev_cookie->expires = aExpires;
    prev_cookie->lastAccessed = get_current_time();
    prev_cookie->isSecure = aSecure;
    prev_cookie->isDomain = aIsDomain;
    prev_cookie->status = aStatus;
    prev_cookie->policy = aPolicy;
    cookieAdded = PR_TRUE;
  } else {
    // construct a new cookie_struct
    if (!cookie_list)
      cookie_list = new nsVoidArray();

    prev_cookie = new cookie_CookieStruct;
    if (prev_cookie) {
      prev_cookie->path = aPath;
      prev_cookie->host = aDomain;
      prev_cookie->name = aName;
      prev_cookie->cookie = aValue;
      prev_cookie->expires = aExpires;
      prev_cookie->lastAccessed = get_current_time();
      prev_cookie->isSecure = aSecure;
      prev_cookie->isDomain = aIsDomain;
      prev_cookie->status = aStatus;
      prev_cookie->policy = aPolicy;
    }

    if (prev_cookie && cookie_list) {
      // add it to the list so that it is before any strings of smaller length
      for (PRInt32 i = cookie_list->Count(); i > 0;) {
        i--;
        cookie_CookieStruct *tmp_cookie_ptr =
              NS_STATIC_CAST(cookie_CookieStruct*, cookie_list->ElementAt(i));
        NS_ASSERTION(tmp_cookie_ptr, "corrupt cookie list");
        if (prev_cookie->path.Length() <= tmp_cookie_ptr->path.Length()) {
          cookie_list->InsertElementAt(prev_cookie, i + 1);
          cookieAdded = PR_TRUE;
          break;
        }
      }
      if (!cookieAdded ) {
        /* no shorter strings found in list */
        cookie_list->InsertElementAt(prev_cookie, 0);
        cookieAdded = PR_TRUE;
      }
    } else {
      delete prev_cookie;
    }
  }

  if (cookieAdded) {
    // Make a note to write the cookies to file
    cookie_changed = PR_TRUE;

    // Notify statusbar if we need to turn on the cookie icon
    if (prev_cookie->status == nsICookie::STATUS_DOWNGRADED ||
        prev_cookie->status == nsICookie::STATUS_FLAGGED) {
      nsCOMPtr<nsIObserverService> os(do_GetService("@mozilla.org/observer-service;1"));
      if (os)
        os->NotifyObservers(nsnull, "cookieIcon", NS_LITERAL_STRING("on").get());
    }
    return NS_OK;
  }
  return NS_ERROR_OUT_OF_MEMORY;
}

MODULE_PRIVATE nsresult
cookie_ParseDate(const nsAFlatCString &date_string, time_t & date) {

  PRTime prdate;
  date = 0;
  // TRACEMSG(("Parsing date string: %s\n",date_string));

  if(PR_ParseTimeString(date_string.get(), PR_TRUE, &prdate) == PR_SUCCESS) {
    if (LL_CMP(prdate, <, LL_Zero())) { // prdate < 0
      return NS_OK; // date = 0 from above
    }
    PRInt64 r, u;
    LL_I2L(u, PR_USEC_PER_SEC);
    LL_DIV(r, prdate, u);
    LL_L2I(date, r);
    if (date < 0) {
      date = MAX_EXPIRE;
    }
    // TRACEMSG(("Parsed date as GMT: %s\n", asctime(gmtime(&date))));    // TRACEMSG(("Parsed date as local: %s\n", ctime(&date)));
    return NS_OK;
  } 
  return NS_ERROR_FAILURE;
}
