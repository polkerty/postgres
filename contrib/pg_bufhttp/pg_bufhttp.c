#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"

/* For referencing Postgres buffer manager internals */
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"  /* Where BufferDesc is defined in many PG versions. */

#include "postmaster/bgworker.h"

PG_MODULE_MAGIC;

/* Forward declarations */
void _PG_init(void);
void _PG_fini(void);
PGDLLEXPORT void start_http_server(Datum main_arg);

/* 
 * Optional: you might want to define these as extern if they're not 
 * automatically visible. For example, older PG might need:
 *
 * extern BufferDesc *BufferDescriptors;
 * extern int NBuffers;
 */

/* 
 * We'll keep a simple global for controlling the server thread. 
 */
static volatile bool shutdown_requested = false;


// static void
// export_buffers_via_http()
// {
//     int i;
    
//     /* 
//      * Potentially: Acquire lock. For instance, 
//      * "LWLockAcquire(BufFreelistLock, LW_SHARED);" or some 
//      * other relevant lock that ensures stable reading. 
//      * But be extremely cautious: the specifics can differ by PG version.
//      */
    
//     for (i = 0; i < NBuffers; i++)
//     {
//         BufferDesc *desc = GetBufferDescriptor(i);
//         /* Or: desc = &BufferDescriptors[i]; in some versions */

//         /* Read metadata safely. For example: */
//         BufferTag tag = desc->tag;
//         pg_atomic_uint32 state = desc->state;

//         /* 
//          * If you want the actual page data, you'd do something like:
//          *    Buffer buf = BufferDescriptorGetBuffer(desc);
//          *    Page page = BufferGetPage(buf);
//          * But be mindful of concurrency and correctness.
//          */
        
//         /* Accumulate this in a JSON buffer, or textual output, etc. */
//     }
    
//     /* LWLockRelease(BufFreelistLock); if you acquired it */
// }


static void
register_http_server_worker(void)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	MemSet(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	strcpy(worker.bgw_library_name, "pg_bufhttp");
	strcpy(worker.bgw_function_name, "start_http_server");
	strcpy(worker.bgw_name, "bufhttp server");
	strcpy(worker.bgw_type, "bufhttp server");

	if (process_shared_preload_libraries_in_progress)
	{
		RegisterBackgroundWorker(&worker);
		return;
	}

    /* must set notify PID to wait for startup */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background process"),
				 errhint("You may need to increase \"max_worker_processes\".")));

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
				 errhint("More details may be available in the server log.")));

}

void
start_http_server(Datum main_arg)
{
    /* This is the entry point for your worker. Set up the server socket, etc. */
    // pqsignal(SIGTERM, MyProcSignalHandler);  /* handle termination signals */
    BackgroundWorkerUnblockSignals();

    /* Run your server loop. On each request, call export_buffers_via_http() */
    printf("pg_bufhttp server");
    printf("pg_bufhttp server: line 2!");
    
}


void
_PG_init(void)
{
    /* 
     * _PG_init() is called when the extension is loaded (if in shared_preload_libraries).
     * Start up the server or background worker here.
     */
    if (!process_shared_preload_libraries_in_progress)
        return;

    register_http_server_worker();
}

void
_PG_fini(void)
{
    /* Clean up anything on unload */
    shutdown_requested = true;
    /* join your thread, etc. */
}
