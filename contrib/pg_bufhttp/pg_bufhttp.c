#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"

/* For referencing Postgres buffer manager internals */
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"  /* Where BufferDesc is defined in many PG versions. */

PG_MODULE_MAGIC;

/* Forward declarations */
void _PG_init(void);
void _PG_fini(void);

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

static void
start_http_server(void)
{
    /* 
     * Spin off a new thread or background worker that:
     *  - listens on a TCP socket (say, port 8081).
     *  - on GET /some-endpoint, calls your function to read BufferDescriptors
     *    and returns them as JSON/whatever.
     */
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

    start_http_server();
}

void
_PG_fini(void)
{
    /* Clean up anything on unload */
    shutdown_requested = true;
    /* join your thread, etc. */
}
