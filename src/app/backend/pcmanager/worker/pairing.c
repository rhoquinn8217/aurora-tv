#include "worker.h"
#include "errors.h"
#include "app.h"

#include "backend/pcmanager/priv.h"
#include "backend/pcmanager/pclist.h"
#include "backend/pcmanager/pair_cancel.h"

#include <errno.h>

int worker_pairing(worker_context_t *context) {
    pcmanager_t *manager = context->manager;
    const pclist_t *node = pcmanager_node(manager, &context->uuid);
    if (node == NULL) {
        return ENOENT;
    }
    GS_CLIENT client = app_gs_client_new(context->app);
    PSERVER_DATA server = serverdata_clone(node->server);
    gs_set_timeout(client, 60);
    /* This fork: a Cancel cuts the wait for the PIN short (pair_cancel.h). */
    pair_cancel_watch(client, context->userdata);
    int ret = gs_pair(client, server, context->arg1);
    pair_cancel_unwatch(client);
    gs_destroy(client);
    if (ret != GS_OK) {
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        context->error = gs_error != NULL ? strdup(gs_error) : NULL;
        serverdata_free(server);
        return ret;
    }
    SERVER_STATE state = {.code = SERVER_STATE_AVAILABLE};
    pclist_upsert(manager, &context->uuid, &state, server);
    return GS_OK;
}