/* Copyright (c) 2009-2012 Stanford University
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

#ifndef RAMCLOUD_REPLICAMANAGER_H
#define RAMCLOUD_REPLICAMANAGER_H

#include <thread>
#include <unordered_map>
#include <boost/pool/pool.hpp>

#include "Common.h"
#include "BoostIntrusive.h"
#include "BackupSelector.h"
#include "ReplicatedSegment.h"
#include "ServerTracker.h"
#include "TaskManager.h"
#include "Tub.h"

namespace RAMCloud {

/**
 * Creates and tracks replicas of local in-memory segments on remote backups.
 *
 * A master's log module issues requests to a ReplicaManager which replicates
 * log data and can be used to free replicas of segments from backups in the
 * cluster.  ReplicaManager also responds to changes in cluster configuration;
 * it restores durability of segments and transparently masks backup failures
 * (both for closed segments and for segments which are actively being written
 * when a backup fails). ReplicaManager tries to mask all failures that can
 * occur in replication (for example, naming, network, or host failures).
 *
 * All operations issued to the ReplicaManager are only queued.  To force all
 * queued operations to complete sync() must be called, otherwise the
 * ReplicaManager casually tries to perform some replication whenever proceed()
 * is called.
 *
 * The log module uses openSegment() to make the ReplicaManager aware of
 * an in-memory segment which must be replicated.  See ReplicatedSegment for
 * details on how the log module informs the ReplicaManager of changes to the
 * in-memory segment image and what guarantees the ReplicaManager provides.
 *
 * There must be exactly one ReplicaManager per log otherwise behavior is
 * undefined.
 */
class ReplicaManager
    : public ReplicatedSegment::Deleter
{
   PUBLIC:
    ReplicaManager(ServerList& serverList,
                   const ServerId& masterId, uint32_t numReplicas);
    ~ReplicaManager();

    ReplicatedSegment* openSegment(uint64_t segmentId,
                                   const void* data, uint32_t openLen);
        __attribute__((warn_unused_result));
    void proceed();

    /// Number replicas to keep of each segment.
    const uint32_t numReplicas;

  PRIVATE:
    void clusterConfigurationChanged();

    /**
     * A ServerTracker used to find backups and track replica distribution
     * stats.  Each entry in the tracker contains a pointer to a BackupStats
     * struct which stores the number of primary replicas stored on that
     * server.  Used by ReplicatedSegments and #backupSelector; updated by
     * #backupSelector.
     */
    BackupTracker tracker;

    /// Selects backups to store replicas while obeying placement constraints.
    BackupSelector backupSelector;

    /**
     * Protects all internal data structures during concurrent calls to the
     * ReplicaManager and any of its ReplicatedSegments.
     * This includes all data being tracked for each individual segment and
     * its replicas as well as helper structures like the #taskManager and
     * #replicatedSegmentList.  A lock for this mutex must be held to read
     * or modify any state in the ReplicaManager.
     */
    std::mutex dataMutex;
    typedef std::lock_guard<std::mutex> Lock;

    /// Id of master that this will be managing replicas for.
    const ServerId& masterId;

    /// Allows fast reuse of ReplicatedSegment allocations.
    boost::pool<> replicatedSegmentPool;

    INTRUSIVE_LIST_TYPEDEF(ReplicatedSegment, listEntries)
        ReplicatedSegmentList;

    /**
     * A list all ReplicatedSegments (one for each segment in the log
     * which hasn't been freed). Newly opened segments are pushed to the back.
     */
    ReplicatedSegmentList replicatedSegmentList;

    /**
     * Enqueues segments that need replication/freeing and makes progress
     * on enqueued operations whenever taskManager.proceed() is called.
     */
    TaskManager taskManager;

    /**
     * Number of collective outstanding write rpcs to all backups.
     * Used by ReplicatedSegment to throttle rpc creation.
     */
    uint32_t writeRpcsInFlight;

  PUBLIC:
    // Only used by ReplicatedSegment.
    void destroyAndFreeReplicatedSegment(ReplicatedSegment* replicatedSegment);

    DISALLOW_COPY_AND_ASSIGN(ReplicaManager);
};

} // namespace RAMCloud

#endif
