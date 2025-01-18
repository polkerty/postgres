#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"

/* For referencing Postgres buffer manager internals */
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"  /* Where BufferDesc is defined in many PG versions. */

#include "postmaster/bgworker.h"

#include "storage/ipc.h"
#include "storage/proc.h"
#include "miscadmin.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

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


// static void PrintBufs(void)
// {
// 	int			i;

// 	for (i = 0; i < NBuffers; ++i)
// 	{
// 		BufferDesc *buf = GetBufferDescriptor(i);
// 		// Buffer		b = BufferDescriptorGetBuffer(buf);

// 		{
// 			/* theoretically we should lock the bufhdr here */
// 			elog(LOG,
// 				 "[%02d] (freeNext=%d, rel=%s, "
// 				 "blockNum=%u, state=0x%x",
// 				 i, buf->freeNext,
// 				 relpathperm(BufTagGetRelFileLocator(&buf->tag),
// 							 BufTagGetForkNum(&buf->tag)),
// 				 buf->tag.blockNum, (uint32) buf->state.value);
// 		}
// 	}
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


/* The server */

/* Forward declarations */
static void handle_client(int client_fd);
static char *export_buffers_as_json(void);

static int BUFF_SIZE = 10000000;


void
start_http_server(Datum main_arg)
{
    struct sockaddr_in server_addr;
    int server_fd;
    int optval = 1;

    /*
     * If you want to handle SIGTERM or other signals gracefully,
     * you can set up signal handlers here.
     *
     * e.g., pqsignal(SIGTERM, MyProcSignalHandler);
     *       BackgroundWorkerUnblockSignals();
     */
    BackgroundWorkerUnblockSignals();

    elog(LOG, "pg_bufhttp: starting minimal HTTP server on port 6565");

    /* Create a TCP socket */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        elog(ERROR, "pg_bufhttp: socket() failed");
        return;
    }

    /* Reuse address/port to avoid "address already in use" */
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &optval, sizeof(optval)) < 0)
    {
        elog(ERROR, "pg_bufhttp: setsockopt() failed");
        close(server_fd);
        return;
    }

    /* Bind to port 6565 on any interface (0.0.0.0) */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port        = htons(6565);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        elog(ERROR, "pg_bufhttp: bind() failed on port 6565");
        close(server_fd);
        return;
    }

    /* Start listening */
    if (listen(server_fd, 10) < 0)
    {
        elog(ERROR, "pg_bufhttp: listen() failed");
        close(server_fd);
        return;
    }

    elog(LOG, "pg_bufhttp: server is now listening on port 6565");

    /*
     * Main accept loop. In a real system, you'd want to:
     *  - handle signals and exit gracefully
     *  - possibly spawn threads or processes to handle connections
     */
    for (;;)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd;

        /* Accept a new client */
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0)
        {
            /* In a real server, check errno for EINTR, etc. */
            elog(WARNING, "pg_bufhttp: accept() failed");
            continue;
        }

        /* Handle client synchronously in this example */
        handle_client(client_fd);

        close(client_fd);

        /* 
         * Optionally check for a termination signal or similar 
         * so the worker can exit gracefully. For instance:
         *
         * if (got_sigterm)
         * {
         *     elog(LOG, "pg_bufhttp: received SIGTERM, shutting down");
         *     break;
         * }
         */
    }

    close(server_fd);
    elog(LOG, "pg_bufhttp: server exiting");
}

/*
 * A simple function to read the client's HTTP request and send a response.
 */
static void
handle_client(int client_fd)
{
    char buffer[1024];
    ssize_t bytes_read;
    char method[16], path[256];
    int content_length;
    size_t response_size;
    char *response;
    int written;

    /* Clear buffers */
    memset(buffer, 0, sizeof(buffer));
    memset(method, 0, sizeof(method));
    memset(path, 0, sizeof(path));

    /*
     * Read the request. This example just reads up to 1023 bytes once.
     * A real server might need a loop to read the entire request or headers.
     */
    bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0)
    {
        return;
    }

    /* Parse the first line of the request to extract method and path */
    /* This is a naive parse. Real parsing is more complicated. */
    sscanf(buffer, "%15s %255s", method, path);

    /* We only handle GET in this toy example */
    if (strcmp(method, "GET") == 0)
    {
        if (strcmp(path, "/bufs") == 0)
        {
            char *json = export_buffers_as_json();
            if (!json) {
                fprintf(stderr, "Error: Failed to generate JSON\n");
                return;
            }

            content_length = strlen(json);
            
            // Allocate response buffer dynamically based on content size
            response_size = content_length + 200; // Extra space for headers
            response = malloc(response_size);
            if (!response) {
                fprintf(stderr, "Error: Memory allocation failed\n");
                free(json);
                return;
            }

            // Construct the response safely
            written = snprintf(response, response_size,
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: %d\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "%s",
                                content_length, json);
            
            if (written < 0 || written >= response_size) {
                fprintf(stderr, "Error: Response formatting failed\n");
                free(response);
                free(json);
                return;
            }

            send(client_fd, response, written, 0);

            free(response);
            free(json);
        }
        else
        {
            /* 404 Not Found */
            const char *not_found =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 13\r\n"
                "Connection: close\r\n"
                "\r\n"
                "404 Not Found\n";
            send(client_fd, not_found, strlen(not_found), 0);
        }
    }
    else
    {
        /* 405 Method Not Allowed */
        const char *method_not_allowed =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 23\r\n"
            "Connection: close\r\n"
            "\r\n"
            "405 Method Not Allowed\n";
        send(client_fd, method_not_allowed, strlen(method_not_allowed), 0);
    }
}

/*
 * For demonstration, returns a static JSON string. You could replace
 * this with calls to your actual data-export function (e.g. the
 * "PrintBufs()" or something that serializes real buffer info to JSON).
 */
static char *
export_buffers_as_json(void)
{


    char *jsonBuffer = malloc(BUFF_SIZE); 
    int i;

    int offset = 0;
    offset += sprintf(jsonBuffer + offset, "[\n");

    elog(LOG,
    	 "NBuffers %d",
    	 NBuffers);

    for (i = 0; i < NBuffers; i++)
    {

        BufferDesc *desc = GetBufferDescriptor(i);

        /* Read metadata safely. For example: */
		uint32 buf_state = desc->state.value;
        int refcount = BUF_STATE_GET_REFCOUNT(buf_state);

        int locked = buf_state & BM_LOCKED;
        int dirty = buf_state & BM_DIRTY;
        int valid = buf_state & BM_VALID;
        int tagValid = buf_state & BM_TAG_VALID;
        int ioInProgress = buf_state & BM_IO_IN_PROGRESS;
        int ioError = buf_state & BM_IO_ERROR;
        int justDirtied = buf_state & BM_JUST_DIRTIED;
        int pinCountWaiter = buf_state & BM_PIN_COUNT_WAITER;
        int checkpointNeeded = buf_state & BM_CHECKPOINT_NEEDED;
        int permanent = buf_state & BM_PERMANENT;

        if (i > 0) {
            offset += sprintf(jsonBuffer + offset, ",\n");
        }

        offset += sprintf(jsonBuffer + offset,
            "  {\n"
            "    \"refcount\": %d,\n"
            "    \"id\": \"%d\",\n"
            "    \"locked\": %s,\n"
            "    \"dirty\": %s,\n"
            "    \"valid\": %s,\n"
            "    \"tagValid\": %s,\n"
            "    \"ioInProgress\": %s,\n"
            "    \"ioError\": %s,\n"
            "    \"justDirtied\": %s,\n"
            "    \"pinCountWaiter\": %s,\n"
            "    \"checkpointNeeded\": %s,\n"
            "    \"permanent\": %s\n"
            "  }",
            refcount,
            desc->buf_id,
            (locked ? "true" : "false"),
            (dirty ? "true" : "false"),
            (valid ? "true" : "false"),
            (tagValid ? "true" : "false"),
            (ioInProgress ? "true" : "false"),
            (ioError ? "true" : "false"),
            (justDirtied ? "true" : "false"),
            (pinCountWaiter ? "true" : "false"),
            (checkpointNeeded ? "true" : "false"),
            (permanent ? "true" : "false")
        );

		// elog(LOG,
		// 	 "[%02d] (freeNext=%d, rel=%s, "
		// 	 "blockNum=%u, refcount=%u %d)",
		// 	 i, buf->freeNext,
		// 	 relpathbackend(BufTagGetRelFileLocator(&buf->tag),
		// 					INVALID_PROC_NUMBER, BufTagGetForkNum(&buf->tag)),
		// 	 buf->tag.blockNum, 
		// 	 BUF_STATE_GET_REFCOUNT(buf_state), GetPrivateRefCount(b));
        /* 
         * If you want the actual page data, you'd do something like:
         *    Buffer buf = BufferDescriptorGetBuffer(desc);
         *    Page page = BufferGetPage(buf);
         * But be mindful of concurrency and correctness.
         */
        
    }

    offset += sprintf(jsonBuffer + offset, "\n]\n");

    // Use snprintf to safely write into the buffer
    // snprintf(result, BUFF_SIZE, "{\"total buffer count\": \"%d\"}", NBuffers);
    

    return  jsonBuffer;
}
