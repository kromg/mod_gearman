/******************************************************************************
 *
 * mod_gearman - distribute checks with gearman
 *
 * Copyright (c) 2010 Sven Nierlein
 *
 *****************************************************************************/

#include "common.h"
#include "gearman.h"
#include "utils.h"


/* create the gearman client */
int create_gearman_client( char ** server_list, gearman_client_st *client ) {
    logger( GM_LOG_TRACE, "create_gearman_client()\n" );

    gearman_return_t ret;

    client = gearman_client_create(client);
    if ( client == NULL ) {
        logger( GM_LOG_ERROR, "Memory allocation failure on client creation\n" );
        return GM_ERROR;
    }

    int x = 0;
    while ( server_list[x] != NULL ) {
        char * server   = strdup( server_list[x] );
        char * server_c = server;
        char * host     = str_token( &server, ':' );
        char * port_val = str_token( &server, 0 );
        in_port_t port  = GM_SERVER_DEFAULT_PORT;
        if(port_val != NULL) {
            port  = ( in_port_t ) atoi( port_val );
        }
        ret = gearman_client_add_server( client, host, port );
        if ( ret != GEARMAN_SUCCESS ) {
            logger( GM_LOG_ERROR, "client error: %s\n", gearman_client_error( client ) );
            free(server_c);
            return GM_ERROR;
        }
        free(server_c);
        x++;
    }

    return GM_OK;
}


/* create a task and send it */
int add_job_to_queue( gearman_client_st *client, char * queue, char * uniq, char * data, int priority, int retries ) {
    gearman_task_st *task = NULL;
    gearman_return_t ret;

    if( priority == GM_JOB_PRIO_LOW ) {
        gearman_client_add_task_low_background( client, task, NULL, queue, uniq, ( void * )data, ( size_t )strlen( data ), &ret );
    }
    if( priority == GM_JOB_PRIO_NORMAL ) {
        gearman_client_add_task_background( client, task, NULL, queue, uniq, ( void * )data, ( size_t )strlen( data ), &ret );
    }
    if( priority == GM_JOB_PRIO_HIGH ) {
        gearman_client_add_task_high_background( client, task, NULL, queue, uniq, ( void * )data, ( size_t )strlen( data ), &ret );
    }

    gearman_client_run_tasks( client );
    if(ret != GEARMAN_SUCCESS || (gearman_client_error(client) != NULL && strcmp(gearman_client_error(client), "") != 0)) { // errno is somehow empty, use error instead
        // maybe its a connection issue, so just wait a little bit
        if(retries > 0) {
            retries--;
            sleep(5);
            return(add_job_to_queue( client, queue, uniq, data, priority, retries));
        }
        // no more retries...
        else {
            logger( GM_LOG_ERROR, "add_job_to_queue() finished with errors: %s\n", gearman_client_error(client) );
            return GM_ERROR;
        }
    }
    return GM_OK;
}
