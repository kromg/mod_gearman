/******************************************************************************
 *
 * mod_gearman - distribute checks with gearman
 *
 * Copyright (c) 2010 Sven Nierlein - sven.nierlein@consol.de
 *
 * This file is part of mod_gearman.
 *
 *  mod_gearman is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  mod_gearman is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with mod_gearman.  If not, see <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/

/* include header */
#include "worker.h"
#include "common.h"
#include "worker_client.h"
#include "utils.h"
#include "worker_logger.h"
#include "gearman.h"

char temp_buffer1[GM_BUFFERSIZE];
char temp_buffer2[GM_BUFFERSIZE];
char hostname[GM_BUFFERSIZE];

gearman_worker_st worker;
gearman_client_st client;

int number_jobs_done = 0;

gm_job_t * current_job;
pid_t current_child_pid;
gm_job_t * exec_job;

int sleep_time_after_error = 1;
int worker_run_mode;


/* callback for task completed */
void worker_client(int worker_mode) {

    logger( GM_LOG_TRACE, "worker client started\n" );

    /* set signal handlers for a clean exit */
    signal(SIGINT, clean_worker_exit);
    signal(SIGTERM,clean_worker_exit);

    worker_run_mode = worker_mode;
    exec_job    = ( gm_job_t * )malloc( sizeof *exec_job );

    // create worker
    if(set_worker(&worker) != GM_OK) {
        logger( GM_LOG_ERROR, "cannot start worker\n" );
        exit( EXIT_FAILURE );
    }

    // create client
    if ( create_client( mod_gm_opt->server_list, &client ) != GM_OK ) {
        logger( GM_LOG_ERROR, "cannot start client\n" );
        exit( EXIT_FAILURE );
    }

    gethostname(hostname, GM_BUFFERSIZE-1);

    worker_loop();

    return;
}

/* main loop of jobs */
void worker_loop() {

    while ( 1 ) {
        gearman_return_t ret;

        // wait three minutes for a job, otherwise exit
        if(worker_run_mode != GM_WORKER_STANDALONE)
            alarm(180);

        ret = gearman_worker_work( &worker );
        if ( ret != GEARMAN_SUCCESS ) {
            logger( GM_LOG_ERROR, "worker error: %s\n", gearman_worker_error( &worker ) );
            gearman_job_free_all( &worker );
            gearman_worker_free( &worker );
            gearman_client_free( &client );

            // sleep on error to avoid cpu intensive infinite loops
            sleep(sleep_time_after_error);
            sleep_time_after_error += 3;
            if(sleep_time_after_error > 60)
                sleep_time_after_error = 60;

            // create new connections
            set_worker( &worker );
            create_client( mod_gm_opt->server_list, &client );
        }
    }

    return;
}


/* get a job */
void *get_job( gearman_job_st *job, void *context, size_t *result_size, gearman_return_t *ret_ptr ) {

    // send start signal to parent
    send_state_to_parent(GM_JOB_START);

    // reset timeout for now, will be set befor execution again
    alarm(0);

    logger( GM_LOG_TRACE, "get_job()\n" );

    // contect is unused
    context = context;

    // set size of result
    *result_size = 0;

    // reset sleep time
    sleep_time_after_error = 1;

    // ignore sigterms while running job
    sigset_t block_mask;
    sigset_t old_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &block_mask, &old_mask);

    /* get the data */
    int wsize = gearman_job_workload_size(job);
    char workload[GM_BUFFERSIZE];
    strncpy(workload, (char*)gearman_job_workload(job), wsize);
    workload[wsize] = '\0';
    logger( GM_LOG_TRACE, "got new job %s\n", gearman_job_handle( job ) );
    logger( GM_LOG_TRACE, "%d +++>\n%s\n<+++\n", strlen(workload), workload );

    // decrypt data
    char * decrypted_data = malloc(GM_BUFFERSIZE);
    char * decrypted_data_c = decrypted_data;
    mod_gm_decrypt(&decrypted_data, workload, mod_gm_opt->transportmode);

    if(decrypted_data == NULL) {
        *ret_ptr = GEARMAN_WORK_FAIL;
        return NULL;
    }
    logger( GM_LOG_TRACE, "%d --->\n%s\n<---\n", strlen(decrypted_data), decrypted_data );

    // set result pointer to success
    *ret_ptr= GEARMAN_SUCCESS;

    exec_job->type                = NULL;
    exec_job->host_name           = NULL;
    exec_job->service_description = NULL;
    exec_job->result_queue        = NULL;
    exec_job->command_line        = NULL;
    exec_job->output              = NULL;
    exec_job->exited_ok           = TRUE;
    exec_job->scheduled_check     = TRUE;
    exec_job->reschedule_check    = TRUE;
    exec_job->return_code         = STATE_OK;
    exec_job->latency             = 0.0;
    exec_job->timeout             = mod_gm_opt->job_timeout;
    exec_job->start_time.tv_sec   = 0L;
    exec_job->start_time.tv_usec  = 0L;

    char *ptr;
    char command[GM_BUFFERSIZE];
    while ( (ptr = strsep(&decrypted_data, "\n" )) != NULL ) {
        char *key   = strsep( &ptr, "=" );
        char *value = strsep( &ptr, "\x0" );

        if ( key == NULL )
            continue;

        if ( value == NULL || !strcmp( value, "") )
            continue;

        if ( !strcmp( key, "host_name" ) ) {
            exec_job->host_name = value;
        } else if ( !strcmp( key, "service_description" ) ) {
            exec_job->service_description = value;
        } else if ( !strcmp( key, "type" ) ) {
            exec_job->type = value;
        } else if ( !strcmp( key, "result_queue" ) ) {
            exec_job->result_queue = value;
        } else if ( !strcmp( key, "check_options" ) ) {
            exec_job->check_options = atoi(value);
        } else if ( !strcmp( key, "scheduled_check" ) ) {
            exec_job->scheduled_check = atoi(value);
        } else if ( !strcmp( key, "reschedule_check" ) ) {
            exec_job->reschedule_check = atoi(value);
        } else if ( !strcmp( key, "latency" ) ) {
            exec_job->latency = atof(value);
        } else if ( !strcmp( key, "start_time" ) ) {
            string2timeval(value, &exec_job->core_start_time);
        } else if ( !strcmp( key, "timeout" ) ) {
            exec_job->timeout = atoi(value);
        } else if ( !strcmp( key, "command_line" ) ) {
            snprintf(command, sizeof(command), "%s 2>&1", value);
            exec_job->command_line = command;
        }
    }

    do_exec_job();

    // start listening to SIGTERMs
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    free(decrypted_data_c);

    // send finish signal to parent
    send_state_to_parent(GM_JOB_END);

    return NULL;
}


/* do some job */
void do_exec_job( ) {
    logger( GM_LOG_TRACE, "do_exec_job()\n" );

    struct timeval start_time,end_time;

    if(exec_job->type == NULL) {
        logger( GM_LOG_ERROR, "discarded invalid job\n" );
        return;
    }
    if(exec_job->command_line == NULL) {
        logger( GM_LOG_ERROR, "discarded invalid job\n" );
        return;
    }

    if ( !strcmp( exec_job->type, "service" ) ) {
        logger( GM_LOG_DEBUG, "got service job: %s - %s\n", exec_job->host_name, exec_job->service_description);
    }
    else if ( !strcmp( exec_job->type, "host" ) ) {
        logger( GM_LOG_DEBUG, "got host job: %s\n", exec_job->host_name);
    }
    else if ( !strcmp( exec_job->type, "event" ) ) {
        logger( GM_LOG_DEBUG, "got eventhandler job\n");
    }

    logger( GM_LOG_TRACE, "timeout %i\n", exec_job->timeout);

    // get the check start time
    gettimeofday(&start_time,NULL);
    exec_job->start_time = start_time;
    int latency = exec_job->core_start_time.tv_sec - start_time.tv_sec;

    // job is too old
    if(latency > mod_gm_opt->max_age) {
        exec_job->return_code   = 3;

        logger( GM_LOG_INFO, "discarded too old %s job: %i > %i\n", exec_job->type, (int)latency, mod_gm_opt->max_age);

        gettimeofday(&end_time, NULL);
        exec_job->finish_time = end_time;

        if ( !strcmp( exec_job->type, "service" ) || !strcmp( exec_job->type, "host" ) ) {
            exec_job->output = "(Could Not Start Check In Time)";
            send_result_back();
        }

        return;
    }

    exec_job->early_timeout = 0;

    // run the command
    logger( GM_LOG_TRACE, "command: %s\n", exec_job->command_line);
    execute_safe_command();

    // record check result info
    gettimeofday(&end_time, NULL);
    exec_job->finish_time = end_time;

    // did we have a timeout?
    if(exec_job->timeout < ((int)end_time.tv_sec - (int)start_time.tv_sec)) {
        exec_job->return_code   = 2;
        exec_job->early_timeout = 1;
        if ( !strcmp( exec_job->type, "service" ) )
            exec_job->output = "(Service Check Timed Out)";
        if ( !strcmp( exec_job->type, "host" ) )
            exec_job->output = "(Host Check Timed Out)";
    }

    if ( !strcmp( exec_job->type, "service" ) || !strcmp( exec_job->type, "host" ) ) {
        send_result_back();
    }

    return;
}


/* execute this command with given timeout */
void execute_safe_command() {
    logger( GM_LOG_TRACE, "execute_safe_command()\n" );

    int pdes[2];
    int return_code;
    char plugin_output[GM_BUFFERSIZE];
    strcpy(plugin_output,"");

    int fork_exec = mod_gm_opt->fork_on_exec;

    // fork a child process
    if(fork_exec == GM_ENABLED) {
        if(pipe(pdes) != 0)
            perror("pipe");

        current_child_pid=fork();

        //fork error
        if( current_child_pid == -1 ) {
            exec_job->output      = "(Error On Fork)";
            exec_job->return_code = 3;
            return;
        }
    }

    /* we are in the child process */
    if( fork_exec == GM_DISABLED || current_child_pid == 0 ){

        /* become the process group leader */
        setpgid(0,0);
        current_child_pid = getpid();

        /* remove all customn signal handler */
        sigset_t mask;
        sigfillset(&mask);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);

        if( fork_exec == GM_ENABLED )
            close(pdes[0]);
        signal(SIGALRM, alarm_sighandler);
        alarm(exec_job->timeout);

        /* run the plugin check command */
        FILE *fp = NULL;
        fp = popen(exec_job->command_line, "r");
        if( fp == NULL ) {
            if( fork_exec == GM_ENABLED ) {
                exit(3);
            } else {
                exec_job->output      = "exec error";
                exec_job->return_code = 3;
                alarm(0);
                return;
            }
        }

        /* get all lines of plugin output - escape newlines */
        char buffer[GM_BUFFERSIZE] = "";
        char output[GM_BUFFERSIZE] = "";
        strcpy(buffer,"");
        strcpy(output,"");
        int size = GM_MAX_OUTPUT;
        while(size > 0 && fgets(buffer,sizeof(buffer)-1,fp)){
            strncat(output, buffer, size);
            size -= strlen(buffer);
        }
        char * buf;
        buf = escape_newlines(output);
        snprintf(plugin_output, sizeof(plugin_output), "%s", buf);
        free(buf);

        /* close the process */
        int pclose_result;
        pclose_result = pclose(fp);
        return_code   = real_exit_code(pclose_result);

        if(fork_exec == GM_ENABLED) {
            if(write(pdes[1], plugin_output, strlen(plugin_output)+1) <= 0)
                perror("write");

            if(pclose_result == -1) {
                char error[GM_BUFFERSIZE];
                snprintf(error, sizeof(error), "error: %s", strerror(errno));
                if(write(pdes[1], error, strlen(error)+1) <= 0)
                    perror("write");
            }

            exit(return_code);
        }
    }

    /* we are the parent */
    if( fork_exec == GM_DISABLED || current_child_pid > 0 ){

        logger( GM_LOG_TRACE, "started check with pid: %d\n", current_child_pid);

        if( fork_exec == GM_ENABLED) {
            close(pdes[1]);

            waitpid(current_child_pid, &return_code, 0);
            return_code = real_exit_code(return_code);
            logger( GM_LOG_TRACE, "finished check from pid: %d with status: %d\n", current_child_pid, return_code);
            /* get all lines of plugin output */
            if(read(pdes[0], plugin_output, sizeof(plugin_output)-1) < 0)
                perror("read");

        }

        /* file not executable? */
        if(return_code == 126) {
            return_code = STATE_CRITICAL;
            strncat( plugin_output, "CRITICAL: Return code of 126 is out of bounds. Make sure the plugin you're trying to run is executable.", sizeof( plugin_output ));
        }
        /* file not found errors? */
        else if(return_code == 127) {
            return_code = STATE_CRITICAL;
            strncat( plugin_output, "CRITICAL: Return code of 127 is out of bounds. Make sure the plugin you're trying to run actually exists.", sizeof( plugin_output ));
        }
        /* signaled */
        else if(return_code >= 128 && return_code < 256) {
            char * signame = nr2signal((int)(return_code-128));
            snprintf( plugin_output, sizeof( plugin_output ), "CRITICAL: Return code of %d is out of bounds. Plugin exited by signal %s", (int)(return_code-128), signame);
            return_code = STATE_CRITICAL;
            free(signame);
        }
        exec_job->output      = strdup(plugin_output);
        exec_job->return_code = return_code;
        if( fork_exec == GM_ENABLED) {
            close(pdes[0]);
        }
    }
    alarm(0);
    current_child_pid = 0;

    return;
}


/* send results back */
void send_result_back() {
    logger( GM_LOG_TRACE, "send_result_back()\n" );

    if(exec_job->result_queue == NULL) {
        return;
    }
    if(exec_job->output == NULL) {
        return;
    }

    logger( GM_LOG_TRACE, "queue: %s\n", exec_job->result_queue );
    temp_buffer1[0]='\x0';
    snprintf( temp_buffer1, sizeof( temp_buffer1 )-1, "host_name=%s\ncore_start_time=%i.%i\nstart_time=%i.%i\nfinish_time=%i.%i\nlatency=%f\nreturn_code=%i\nexited_ok=%i\n",
              exec_job->host_name,
              ( int )exec_job->core_start_time.tv_sec,
              ( int )exec_job->core_start_time.tv_usec,
              ( int )exec_job->start_time.tv_sec,
              ( int )exec_job->start_time.tv_usec,
              ( int )exec_job->finish_time.tv_sec,
              ( int )exec_job->finish_time.tv_usec,
              exec_job->latency,
              exec_job->return_code,
              exec_job->exited_ok
            );

    if(exec_job->service_description != NULL) {
        temp_buffer2[0]='\x0';
        strncat(temp_buffer2, "service_description=", (sizeof(temp_buffer2)-1));
        strncat(temp_buffer2, exec_job->service_description, (sizeof(temp_buffer2)-1));
        strncat(temp_buffer2, "\n", (sizeof(temp_buffer2)-1));

        strncat(temp_buffer1, temp_buffer2, (sizeof(temp_buffer1)-1));
    }

    if(exec_job->output != NULL) {
        temp_buffer2[0]='\x0';
        strncat(temp_buffer2, "output=", (sizeof(temp_buffer2)-1));
        if(mod_gm_opt->debug_result) {
            strncat(temp_buffer2, "(", (sizeof(temp_buffer2)-1));
            strncat(temp_buffer2, hostname, (sizeof(temp_buffer2)-1));
            strncat(temp_buffer2, ") - ", (sizeof(temp_buffer2)-1));
        }
        strncat(temp_buffer2, exec_job->output, (sizeof(temp_buffer2)-1));
        strncat(temp_buffer2, "\n", (sizeof(temp_buffer2)-1));
        strncat(temp_buffer1, temp_buffer2, (sizeof(temp_buffer1)-1));
        free(exec_job->output);
    }
    strncat(temp_buffer1, "\n", (sizeof(temp_buffer1)-2));

    logger( GM_LOG_TRACE, "data:\n%s\n", temp_buffer1);

    if(add_job_to_queue( &client,
                         mod_gm_opt->server_list,
                         exec_job->result_queue,
                         NULL,
                         temp_buffer1,
                         GM_JOB_PRIO_NORMAL,
                         GM_DEFAULT_JOB_RETRIES,
                         mod_gm_opt->transportmode
                        ) == GM_OK) {
        logger( GM_LOG_TRACE, "send_result_back() finished successfully\n" );
    }
    else {
        logger( GM_LOG_TRACE, "send_result_back() finished unsuccessfully\n" );
    }

    return;
}


/* create the worker */
int set_worker( gearman_worker_st *worker ) {
    logger( GM_LOG_TRACE, "set_worker()\n" );

    create_worker( mod_gm_opt->server_list, worker );

    if(mod_gm_opt->hosts == GM_ENABLED)
        worker_add_function( worker, "host", get_job );

    if(mod_gm_opt->services == GM_ENABLED)
        worker_add_function( worker, "service", get_job );

    if(mod_gm_opt->events == GM_ENABLED)
        worker_add_function( worker, "eventhandler", get_job );

    int x = 0;
    while ( mod_gm_opt->hostgroups_list[x] != NULL ) {
        char buffer[GM_BUFFERSIZE];
        snprintf( buffer, (sizeof(buffer)-1), "hostgroup_%s", mod_gm_opt->hostgroups_list[x] );
        worker_add_function( worker, buffer, get_job );
        x++;
    }

    x = 0;
    while ( mod_gm_opt->servicegroups_list[x] != NULL ) {
        char buffer[GM_BUFFERSIZE];
        snprintf( buffer, (sizeof(buffer)-1), "servicegroup_%s", mod_gm_opt->servicegroups_list[x] );
        worker_add_function( worker, buffer, get_job );
        x++;
    }

    // add our dummy queue, gearman sometimes forgets the last added queue
    worker_add_function( worker, "dummy", dummy);

    return GM_OK;
}


/* called when check runs into timeout */
void alarm_sighandler(int sig) {
    logger( GM_LOG_TRACE, "alarm_sighandler(%i)\n", sig );

    pid_t pid = getpid();
    signal(SIGINT, SIG_IGN);
    logger( GM_LOG_TRACE, "send SIGINT to %d\n", pid);
    kill(-pid, SIGINT);
    signal(SIGINT, SIG_DFL);
    sleep(1);
    logger( GM_LOG_TRACE, "send SIGKILL to %d\n", pid);
    kill(-pid, SIGKILL);

    if(worker_run_mode != GM_WORKER_STANDALONE)
        _exit(EXIT_SUCCESS);

    return;
}

/* tell parent our state */
void send_state_to_parent(int status) {
    logger( GM_LOG_TRACE, "send_state_to_parent(%d)\n", status );

    if(worker_run_mode == GM_WORKER_STANDALONE)
        return;

    int shmid;
    int *shm;

    // Locate the segment.
    if ((shmid = shmget(mod_gm_shm_key, GM_SHM_SIZE, 0666)) < 0) {
        perror("shmget");
        logger( GM_LOG_TRACE, "worker finished: %d\n", getpid() );
        exit( EXIT_FAILURE );
    }

    // Now we attach the segment to our data space.
    if ((shm = shmat(shmid, NULL, 0)) == (int *) -1) {
        perror("shmat");
        logger( GM_LOG_TRACE, "worker finished: %d\n", getpid() );
        exit( EXIT_FAILURE );
    }

    // set our counter
    if(status == GM_JOB_START)
        shm[0]++;
    if(status == GM_JOB_END)
        shm[0]--;

    // detach from shared memory
    if(shmdt(shm) < 0)
        perror("shmdt");

    // wake up parent
    kill(getppid(), SIGUSR1);

    if(number_jobs_done >= GM_MAX_JOBS_PER_CLIENT) {
        logger( GM_LOG_TRACE, "worker finished: %d\n", getpid() );
        exit(EXIT_SUCCESS);
    }

    return;
}


/* do a clean exit */
void clean_worker_exit(int sig) {
    logger( GM_LOG_TRACE, "clean_worker_exit(%d)\n", sig);

    gearman_job_free_all( &worker );
    gearman_worker_free( &worker );
    gearman_client_free( &client );

    exit( EXIT_SUCCESS );
}