/* Copyright (c) 2011-2012 Stanford University
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

#ifndef RAMCLOUD_BACKUPSELECTOR_H
#define RAMCLOUD_BACKUPSELECTOR_H

#include "Common.h"
#include "ServerTracker.h"

namespace RAMCloud {

/**
 * Tracks speed of backups and count of replicas stored on each which is
 * used to balance placement of replicas across the cluster; stored for
 * backup in a BackupTracker.
 */
struct BackupStats {
    BackupStats()
        : primaryReplicaCount(0)
        , expectedReadMBytesPerSec(0)
    {}

    uint32_t getExpectedReadMs();

    /// Number of primary replicas this master has stored on the backup.
    uint32_t primaryReplicaCount;

    /// Disk bandwidth of the host in MB/s
    uint32_t expectedReadMBytesPerSec;
};

/// Tracks BackupStats; a ReplicaManager processes ServerListChanges.
typedef ServerTracker<BackupStats> BackupTracker;

/**
 * See BackupSelector; base class only used to virtualize some calls for testing.
 */
class BaseBackupSelector {
  PUBLIC:
    virtual ServerId selectPrimary(uint32_t numBackups,
                                   const ServerId backupIds[]) = 0;
    virtual ServerId selectSecondary(uint32_t numBackups,
                                     const ServerId backupIds[]) = 0;
    virtual ~BaseBackupSelector() {}
};

/**
 * Selects backups on which to store replicas while obeying replica placement
 * constraints and balancing expected work among backups for recovery.
 * Logically part of the ReplicaManager.
 */
class BackupSelector : public BaseBackupSelector {
  PUBLIC:

    explicit BackupSelector(BackupTracker& tracker);
    ServerId selectPrimary(uint32_t numBackups, const ServerId backupIds[]);
    ServerId selectSecondary(uint32_t numBackups, const ServerId backupIds[]);

  PRIVATE:
    void applyTrackerChanges();
    bool conflict(const ServerId backupId,
                  const ServerId otherBackupId) const;
    bool conflictWithAny(const ServerId backupId,
                         uint32_t numBackups,
                         const ServerId backupIds[]) const;


    /**
     * A ServerTracker used to find backups and track replica distribution
     * stats.  Each entry in the tracker contains a pointer to a BackupStats
     * struct which stores the number of primary replicas stored on that
     * server.
     */
    BackupTracker& tracker;

    DISALLOW_COPY_AND_ASSIGN(BackupSelector);
};

} // namespace RAMCloud

#endif
