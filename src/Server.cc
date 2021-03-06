/* Copyright (c) 2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "BindTransport.h"
#include "Server.h"
#include "ServiceManager.h"

namespace RAMCloud {

/**
 * Create services according to #config, enlist with the coordinator and
 * then return. This method should almost exclusively be used by MockCluster
 * and is only useful for unit testing.  Production code should always use
 * run() instead.
 *
 * \param bindTransport
 *      The BindTransport to register on to listen for rpcs during unit
 *      testing.
 */
void
Server::startForTesting(BindTransport& bindTransport)
{
    createAndRegisterServices(&bindTransport);
    enlist();
}

/**
 * Create services according to #config and enlist with the coordinator.
 * Either call this method or startForTesting(), not both.  Loops
 * forever calling Dispatch::poll() to serve requests.
 */
void
Server::run()
{
    createAndRegisterServices(NULL);

    // Only pin down memory _after_ users of LargeBlockOfMemory have
    // obtained their allocations (since LBOM probes are much slower if
    // the memory needs to be pinned during mmap).
    pinAllMemory();

    // The following statement suppresses a "long gap" message that would
    // otherwise be generated by the next call to dispatch.poll (the
    // warning is benign, and is caused by the time to benchmark secondary
    // storage above.
    Dispatch& dispatch = *Context::get().dispatch;
    dispatch.currentTime = Cycles::rdtsc();

    enlist();

    while (true)
        dispatch.poll();
}

// - private -

/**
 * Create each of the services which are marked as active in config.services,
 * configure them according to #config, and register them with the
 * ServiceManager (or, if bindTransport is supplied, with the transport).
 *
 * \param bindTransport
 *      If given register the services with \a bindTransport instead of
 *      the Context's ServiceManager.
 */
void
Server::createAndRegisterServices(BindTransport* bindTransport)
{
    if (config.services.has(COORDINATOR_SERVICE)) {
        DIE("Server class is not capable of running the CoordinatorService "
            "(yet).");
    }

    coordinator.construct(config.coordinatorLocator.c_str());

    if (config.services.has(MASTER_SERVICE)) {
        LOG(NOTICE, "Master is using %u backups", config.master.numReplicas);
        master.construct(config, coordinator.get(), serverList);
        if (bindTransport) {
            bindTransport->addService(*master,
                                      config.localLocator,
                                      MASTER_SERVICE);
        } else {
            Context::get().serviceManager->addService(*master,
                                                      MASTER_SERVICE);
        }
    }

    if (config.services.has(BACKUP_SERVICE)) {
        backup.construct(config);
        if (config.backup.mockSpeed == 0) {
            backup->benchmark(backupReadSpeed, backupWriteSpeed);
        } else {
            backupReadSpeed = backupWriteSpeed = config.backup.mockSpeed;
        }
        if (bindTransport) {
            bindTransport->addService(*backup,
                                      config.localLocator,
                                      BACKUP_SERVICE);
        } else {
            Context::get().serviceManager->addService(*backup,
                                                      BACKUP_SERVICE);
        }
    }

    if (config.services.has(MEMBERSHIP_SERVICE)) {
        membership.construct(serverId, serverList);
        if (bindTransport) {
            bindTransport->addService(*membership,
                                      config.localLocator,
                                      MEMBERSHIP_SERVICE);
        } else {
            Context::get().serviceManager->addService(*membership,
                                                      MEMBERSHIP_SERVICE);
        }
    }

    if (config.services.has(PING_SERVICE)) {
        ping.construct(&serverList);
        if (bindTransport) {
            bindTransport->addService(*ping,
                                      config.localLocator,
                                      PING_SERVICE);
        } else {
            Context::get().serviceManager->addService(*ping,
                                                       PING_SERVICE);
        }
    }
}

/**
 * Enlist the Server with the coordinator and start the failure detector
 * if it is enabled in #config.
 */
void
Server::enlist()
{
    // Enlist with the coordinator just before dedicating this thread
    // to rpc dispatch. This reduces the window of being unavailable to
    // service rpcs after enlisting with the coordinator (which can
    // lead to session open timeouts).
    serverId = coordinator->enlistServer(config.services,
                                         config.localLocator,
                                         backupReadSpeed, backupWriteSpeed);

    if (master)
        master->init(serverId);
    if (backup)
        backup->init(serverId);
    if (config.detectFailures) {
        failureDetector.construct(
            config.coordinatorLocator,
            serverId,
            serverList);
        failureDetector->start();
    }
}

} // namespace RAMCloud
