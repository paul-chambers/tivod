/**
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>

#include <avahi-common/thread-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

#define loginfo(...)   syslog( LOG_INFO, __VA_ARGS__ )
#define logerror(...)  syslog( LOG_ERR,  __VA_ARGS__ )


typedef struct TiVoUnit {
    struct TiVoUnit *next;
    char   *name;
    char   *serial;
    char   *address;
} TiVoUnit;

/* root of the list of tivos discovered */
TiVoUnit *tivoUnits = NULL;

static AvahiThreadedPoll *bonjourThread = NULL;


/****************************************/








/****************************************/


TiVoUnit *rememberTiVo( const char *name )
{
    TiVoUnit *tivo;

    tivo = malloc( sizeof(TiVoUnit) );

    tivo->name = strdup( name );
    tivo->address = NULL;
    tivo->serial  = NULL;

    /*-- begin critical section --*/
    tivo->next = tivoUnits;
    tivoUnits  = tivo;
    /*-- end critical section --*/

    return tivo;
}

void forgetTiVo( TiVoUnit *tivo )
{
    if ( tivo != NULL )
    {
        if ( tivo->name != NULL ) free(tivo->name);
        if ( tivo->serial != NULL ) free(tivo->serial);
        if ( tivo->address != NULL ) free(tivo->address);
        free(tivo);
    }
}


static void resolveCallback( AvahiServiceResolver *r,
                             AVAHI_GCC_UNUSED AvahiIfIndex interface,
                             AVAHI_GCC_UNUSED AvahiProtocol protocol,
                             AvahiResolverEvent event,
                             const char *name,
                             const char *type,
                             const char *domain,
                             const char *host_name,
                             const AvahiAddress *address,
                             uint16_t port,
                             AvahiStringList *txt,
                             AvahiLookupResultFlags flags,
                             void* userdata )
{
    TiVoUnit        *tivo = userdata;
    char             addr[AVAHI_ADDRESS_STR_MAX];
    AvahiStringList *list;
    char            *key, *value;

    assert(r);

    /* Called whenever a service has been resolved successfully, or timed out */

    switch (event)
    {
    case AVAHI_RESOLVER_FAILURE:
        logerror( "Failed to resolve service '%s' of type '%s' in domain '%s': %s\n",
                  name, type, domain, avahi_strerror( avahi_client_errno( avahi_service_resolver_get_client(r) ) ) );
        break;

    case AVAHI_RESOLVER_FOUND:
        /* now we also have the TiVo's serial number and network address */

        avahi_address_snprint( addr, sizeof(addr), address );
        tivo->address = strdup( addr );

        list = avahi_string_list_find( txt, "TSN" );
        avahi_string_list_get_pair( list, &key, &value, NULL );
        tivo->serial = strdup( value );

        avahi_free( key );
        avahi_free( value );

        loginfo( "Resolved '%s' to TSN '%s' at %s", tivo->name, tivo->serial, tivo->address );

        break;
    }

    /* all done with the resolver, so free it */
    avahi_service_resolver_free( r );
}

static void browseCallback( AvahiServiceBrowser *b,
                            AvahiIfIndex interface,
                            AvahiProtocol protocol,
                            AvahiBrowserEvent event,
                            const char *name,
                            const char *type,
                            const char *domain,
                            AVAHI_GCC_UNUSED AvahiLookupResultFlags flags,
                            void* userdata )
{
    AvahiClient *c = userdata;
    TiVoUnit    *tivo, *prev, *next;

    /* Called whenever a new tivo becomes available on the LAN or is removed from the LAN */

    switch (event)
    {
    case AVAHI_BROWSER_NEW:  /* detected a new TiVo on the LAN */

        tivo = rememberTiVo( name );

        /* We don't save the resolver object returned by avahi_service_resolver_new().
           Instead, when the resolver calls the callback function with the results, the new
           TiVoUnit is added, then the resolver is freed. If the server terminates before
           the callback function is called, the server will free the resolver for us. */

        if ( !avahi_service_resolver_new( c, interface, protocol,
                                          name, type, domain,
                                          AVAHI_PROTO_UNSPEC, 0,
                                          resolveCallback, tivo) )
        {
            logerror( "Failed to resolve service '%s': %s", name, avahi_strerror(avahi_client_errno(c)) );
        }
        break;

    case AVAHI_BROWSER_REMOVE:  /* a TiVo disappeared from the LAN */

        loginfo( "TiVo '%s' disappeared from network", name );

        /*-- begin critical section --*/
        prev = NULL;
        tivo = tivoUnits;
        while ( tivo != NULL )
        {
            next = tivo->next;
            if ( !strcmp( tivo->name, name ) )
            {   /* found it, so unlink it */
                if ( prev == NULL )
                     tivoUnits  = next;
                else prev->next = next;
                /* prev itself stays put */
                forgetTiVo( tivo );
            }
            else prev = tivo;

            tivo = next;
        }
        /*-- end critical section --*/
        break;

    case AVAHI_BROWSER_CACHE_EXHAUSTED:
        loginfo( "(Browser) cache exhausted" );
        break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
        loginfo( "(Browser) all for now" );
        break;

    case AVAHI_BROWSER_FAILURE:
        logerror( "(Browser) %s", avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(b))) );
        avahi_threaded_poll_quit( bonjourThread );
        return;
    }
}


static void client_callback( AvahiClient *c, AvahiClientState state, AVAHI_GCC_UNUSED void * userdata )
{
    /* Called whenever the client or server state changes */

    if (state == AVAHI_CLIENT_FAILURE) {
        logerror( "Server connection failure: %s", avahi_strerror( avahi_client_errno(c) ) );
        avahi_threaded_poll_quit(bonjourThread);
    }
}


int dumpTiVos(void)
{
    TiVoUnit *tivo = tivoUnits;

    while (tivo != NULL)
    {
        fprintf(stderr, "Found '%s' with serial '%s' at %s\n", tivo->name, tivo->serial, tivo->address );
        tivo = tivo->next;
    }

    return 0;
}

int main( AVAHI_GCC_UNUSED int argc, char *argv[] )
{
    AvahiClient *client = NULL;
    AvahiServiceBrowser *serviceBrowser = NULL;
    char       *myName;
    int         error;
    int         result = 1;
    TiVoUnit   *tivo;

    myName = strrchr(argv[0], '/') + 1;
    if (myName == (NULL+1))
        myName = argv[0];

    openlog( myName, LOG_PID, LOG_DAEMON );
    loginfo("started");

    /* must be done before any threads start */
    curl_global_init( CURL_GLOBAL_ALL );

    /* Allocate main loop object */
    bonjourThread = avahi_threaded_poll_new();
    if ( !bonjourThread )
    {
        logerror( "Failed to create threaded poll object." );
    }
    else
    {
        /* Allocate a new client */
        client = avahi_client_new( avahi_threaded_poll_get( bonjourThread ), 0, client_callback, NULL, &error );

        /* Check wether creating the client object succeeded */
        if ( !client )
        {
            logerror( "Unable to create client: %s", avahi_strerror( error ) );
        }
        else
        {
            /* Create the service browser */
            serviceBrowser = avahi_service_browser_new( client,
                                                        AVAHI_IF_UNSPEC,
                                                        AVAHI_PROTO_UNSPEC,
                                                        "_tivo-device._tcp",
                                                        NULL, 0,
                                                        browseCallback, client );
            if ( !serviceBrowser )
            {
                logerror( "Unable to create service browser: %s", avahi_strerror( avahi_client_errno( client ) ) );
            }
            else
            {
                /* start the background network scan for TiVos */
                avahi_threaded_poll_start( bonjourThread );

                /* hack: wait for some results to arrive */
                sleep(5);
                /* what did we find? */
                result = dumpTiVos();

                avahi_threaded_poll_stop( bonjourThread );
                avahi_service_browser_free( serviceBrowser );
            }
            avahi_client_free( client );
        }
        avahi_threaded_poll_free( bonjourThread );
    }

    /* docs say no other threads may be running */
    curl_global_cleanup();

    loginfo( "stopped" );
    closelog();

    return result;
}
