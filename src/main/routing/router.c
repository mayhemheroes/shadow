/*
 * shd-router.c
 *
 * This component models the upstream (ISP) router from a host's external facing
 * network interface. The router uses a queue management algorithm to smooth out
 * packet bursts from fast networks onto slow networks.
 *
 *  Created on: Jan 7, 2020
 *      Author: rjansen
 */

#include <glib.h>

#include "lib/logger/logger.h"
#include "main/core/support/definitions.h"
#include "main/core/worker.h"
#include "main/host/network_interface.h"
#include "main/routing/packet.h"
#include "main/routing/router.h"
#include "main/routing/router_queue_codel.h"
#include "main/routing/router_queue_single.h"
#include "main/routing/router_queue_static.h"
#include "main/utility/utility.h"

struct _Router {
    /* the algorithm we use to manage the router queue */
    QueueManagerMode queueMode;
    const QueueManagerHooks* queueHooks;
    void* queueManager;

    /* the interface that we deliver packets to */
    NetworkInterface* interface;

    gint referenceCount;
    MAGIC_DECLARE;
};

Router* router_new(QueueManagerMode queueMode, NetworkInterface* interface) {
    utility_debugAssert(interface);

    Router* router = g_new0(Router, 1);
    MAGIC_INIT(router);

    router->queueMode = queueMode;
    router->interface = interface;

    router->referenceCount = 1;

    if(router->queueMode == QUEUE_MANAGER_SINGLE) {
        router->queueHooks = routerqueuesingle_getHooks();
    } else if(router->queueMode == QUEUE_MANAGER_STATIC) {
        router->queueHooks = routerqueuestatic_getHooks();
    } else if(router->queueMode == QUEUE_MANAGER_CODEL) {
        router->queueHooks = routerqueuecodel_getHooks();
    } else {
        utility_panic("Queue manager mode %i is undefined", (int)queueMode);
    }

    utility_debugAssert(router->queueHooks->new);
    utility_debugAssert(router->queueHooks->free);
    utility_debugAssert(router->queueHooks->enqueue);
    utility_debugAssert(router->queueHooks->dequeue);
    utility_debugAssert(router->queueHooks->peek);

    router->queueManager = router->queueHooks->new();

    worker_count_allocation(Router);
    return router;
}

static void _router_free(Router* router) {
    MAGIC_ASSERT(router);

    router->queueHooks->free(router->queueManager);

    MAGIC_CLEAR(router);
    g_free(router);
    worker_count_deallocation(Router);
}

void router_ref(Router* router) {
    MAGIC_ASSERT(router);
    (router->referenceCount)++;
}

void router_unref(Router* router) {
    MAGIC_ASSERT(router);
    (router->referenceCount)--;
    utility_debugAssert(router->referenceCount >= 0);
    if(router->referenceCount == 0) {
        _router_free(router);
    }
}

void router_forward(Router* router, Host* src, Packet* packet) {
    MAGIC_ASSERT(router);
    /* just immediately forward the sending task to the worker, who will compute the
     * path and the appropriate delays to the destination. The packet will arrive
     * at the destination's router after a delay equal to the network latency.  */
    worker_sendPacket(src, packet);
}

void router_enqueue(Router* router, Host* host, Packet* packet) {
    MAGIC_ASSERT(router);
    utility_debugAssert(packet);

    Packet* bufferedPacket = router->queueHooks->peek(router->queueManager);

    gboolean wasQueued = router->queueHooks->enqueue(router->queueManager, packet);

    if(wasQueued) {
        packet_addDeliveryStatus(packet, PDS_ROUTER_ENQUEUED);
    } else {
        packet_addDeliveryStatus(packet, PDS_ROUTER_DROPPED);
    }

    /* notify the netiface that we have a new packet so it can dequeue it. */
    if(!bufferedPacket && wasQueued) {
        networkinterface_receivePackets(router->interface, host);
    }
}

Packet* router_dequeue(Router* router) {
    MAGIC_ASSERT(router);

    Packet* packet = router->queueHooks->dequeue(router->queueManager);
    if(packet) {
        packet_addDeliveryStatus(packet, PDS_ROUTER_DEQUEUED);
    }

    return packet;
}

Packet* router_peek(Router* router) {
    MAGIC_ASSERT(router);
    return router->queueHooks->peek(router->queueManager);
}
