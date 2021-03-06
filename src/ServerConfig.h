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

#ifndef RAMCLOUD_SERVERCONFIG_H
#define RAMCLOUD_SERVERCONFIG_H

#include "Log.h"
#include "Segment.h"
#include "ServiceMask.h"

namespace RAMCloud {

/**
 * Configuration details for a Server; describes which coordinator to enlist
 * with, which services to run and advertise, and the configuration details
 * for each of those services.
 *
 * Used for command-line arguments along with some additional details.  Also
 * used for configuring lightweight servers for unit testing (see MockCluster).
 *
 * Instances for unit-testing are created using the forTesting() static method
 * which tries to provide some sane defaults for testing.
 */
struct ServerConfig {
  private:
    /// Piece of junk used internally to select between (private) constructors.
    static struct Testing {} testing;

    /**
     * Private constructor used to generate a ServerConfig well-suited to most
     * unit tests.  See forTesting() to actually create a ServerConfig.
     */
    ServerConfig(Testing) // NOLINT
        : coordinatorLocator()
        , localLocator()
        , services{MASTER_SERVICE, BACKUP_SERVICE,
                   MEMBERSHIP_SERVICE}
        , detectFailures(false)
        , pinMemory(false)
        , master(testing)
        , backup(testing)
    {}

    /**
     * Private constructor used to generate a ServerConfig for running a real
     * RAMCloud server.  The defaults here mean very little; main() sets
     * these fields explicitly.  See forExecution() to actually create a
     * ServerConfig.
     */
    ServerConfig()
        : coordinatorLocator()
        , localLocator()
        , services{MASTER_SERVICE, BACKUP_SERVICE,
                   PING_SERVICE, MEMBERSHIP_SERVICE}
        , detectFailures(true)
        , pinMemory(true)
        , master()
        , backup()
    {}

  public:
    /**
     * Return a ServerConfig which is well-suited for most unit tests.
     * Callers will want to override some of the fields before passing it
     * on to MockCluster::addServer().  In particular, it is important
     * to select which services will run as part of the server via the
     * #services field.
     *
     * See ServerConfig(Testing) for the default values.
     */
    static ServerConfig forTesting() {
        return {testing};
    }

    /**
     * Return a ServerConfig which suited for running a real RAMCloud
     * server; used in main() to hold many of the command-line options.
     *
     * See ServerConfig() for the default values, though these mean little
     * since main() sets the fields explicitly based on command-line
     * args.
     */
    static ServerConfig forExecution() {
        return {};
    }

    /// A locator the server can use to contact the cluster coordinator.
    string coordinatorLocator;

    /// The locator the server should listen for incoming messages on.
    string localLocator;

    /// Which services this server should run and advertise to the cluster.
    ServiceMask services;

    /// Whether the failure detection thread should be started.
    bool detectFailures;

    /**
     * Whether the entire address space of the server should be pinned
     * after initialization has finished.  This can take awhile so we want
     * to skip it for unit tests.
     */
    bool pinMemory;

    /**
     * Configuration details specific to the MasterService on a server,
     * if any.  If !config.has(MASTER_SERVICE) then this field is ignored.
     */
    struct Master {
        /**
         * Constructor used to generate a configuration well-suited to most
         * unit tests.
         */
        Master(Testing) // NOLINT
            : logBytes(32 * 1024 * 1024)
            , hashTableBytes(1 * 1024 * 1024)
            , disableLogCleaner(true)
            , numReplicas(0)
        {}

        /**
         * Constructor used to generate a configuration for running a real
         * RAMCloud server.  The values here are mostly irrelevant since
         * fields are set explicitly from command-line arguments in main().
         */
        Master()
            : logBytes()
            , hashTableBytes()
            , disableLogCleaner()
            , numReplicas()
        {}

        /// Total number bytes to use for the in-memory Log.
        uint64_t logBytes;

        /// Total number of bytes to use for the HashTable.
        uint64_t hashTableBytes;

        /// If true, disable the log cleaner entirely.
        bool disableLogCleaner;

        /// Number of replicas to keep per segment stored on backups.
        uint32_t numReplicas;
    } master;

    /**
     * Configuration details specific to the BackupService a server,
     * if any.  If !config.has(BACKUP_SERVICE) then this field is ignored.
     */
    struct Backup {
        /**
         * Constructor used to generate a configuration well-suited to most
         * unit tests.
         */
        Backup(Testing) // NOLINT
            : inMemory(true)
            , numSegmentFrames(4)
            , segmentSize(64 * 1024)
            , file()
            , strategy(1)
            , mockSpeed(100)
        {}

        /**
         * Constructor used to generate a configuration for running a real
         * RAMCloud server.  The values here are mostly irrelevant since
         * fields are set explicitly from command-line arguments in main().
         */
        Backup()
            : inMemory(false)
            , numSegmentFrames(512)
            , segmentSize(Segment::SEGMENT_SIZE)
            , file("/var/tmp/backup.log")
            , strategy(1)
            , mockSpeed(0)
        {}

        /// Whether the BackupService should store replicas in RAM or on disk.
        bool inMemory;

        /**
         * Number of replica-sized storage chunks to allocate on the backup's
         * backing store.
         */
        uint32_t numSegmentFrames;

        /**
         * Size of segment replicas which will be stored on the backup.
         * TODO(stutsman): This is actually a bug.  Right now the master and
         * backup services don't share the segment size because the master
         * service has some constants baked in.  We need to clean that up
         * and move segmentSize up one level in the config struct so
         * that masters and backups are in agreement.  This could also
         * help us speed up master creation in unit tests.
         */
        uint32_t segmentSize;

        /// Path to a file to use for the backing store if inMemory is false.
        string file;

        /**
         * BackupStrategy to use for balancing replicas across backups.
         * Backups communicate this choice back to MasterServices.
         */
        int strategy;

        /**
         * If mockSpeed is 0 then benchmark the backup's storage and report
         * that to MasterServices.
         * If mockSpeed is non-0 then skip the (slow) benchmarking phase and
         * just report the performance as mockSpeed; in MB/s.
         */
        uint32_t mockSpeed;
    } backup;

  public:
    /**
     * Used in option parsing in main() to help set master.logBytes and
     * master.hashTableBytes.  Only here because there isn't a better place for
     * it.
     *
     * Figure out the Master Server's memory requirements. This means computing
     * the number of bytes to use for the log and the hash table.
     *
     * The user may dictate these parameters by specifying the total memory
     * given to the server, as well as the amount of that to spend on the hash
     * table. The rest is given to the log.
     *
     * Both parameters are string options. If a "%" character is present, they
     * are interpreted as percentages, otherwise they are interpreted as
     * megabytes.
     *
     * \param[in] masterTotalMemory
     *      A string representing the total amount of memory allocated to the
     *      Server. E.g.: "10%" means 10 percent of total system memory, whereas
     *      "256" means 256 megabytes. Only integer quantities are acceptable.
     * \param[in] hashTableMemory
     *      The amount of masterTotalMemory to be used for the hash table. This
     *      may also be a percentage, as above. Only integer quantities are
     *      acceptable.
     * \throw Exception
     *      An exception is thrown if the parameters given are invalid, or
     *      if the total system memory cannot be determined.
     */
    void
    setLogAndHashTableSize(string masterTotalMemory, string hashTableMemory)
    {
        uint64_t masterBytes, hashTableBytes;

        if (masterTotalMemory.find("%") != string::npos) {
            string str = masterTotalMemory.substr(
                0, masterTotalMemory.find("%"));
            uint64_t pct = strtoull(str.c_str(), NULL, 10);
            if (pct <= 0 || pct > 90)
                throw Exception(HERE,
                    "invalid `MasterTotalMemory' option specified: "
                    "not within range 1-90%");
            masterBytes = getTotalSystemMemory();
            if (masterBytes == 0) {
                throw Exception(HERE,
                    "Cannot determine total system memory - "
                    "`MasterTotalMemory' option must not be used");
            }
            masterBytes = (masterBytes * pct) / 100;
        } else {
            masterBytes = strtoull(masterTotalMemory.c_str(), NULL, 10);
            masterBytes *= (1024 * 1024);
        }

        if (hashTableMemory.find("%") != string::npos) {
            string str = hashTableMemory.substr(0, hashTableMemory.find("%"));
            uint64_t pct = strtoull(str.c_str(), NULL, 10);
            if (pct <= 0 || pct > 50) {
                throw Exception(HERE,
                    "invalid HashTableMemory option specified: "
                    "not within range 1-50%");
            }
            hashTableBytes = (masterBytes * pct) / 100;
        } else {
            hashTableBytes = strtoull(hashTableMemory.c_str(), NULL, 10);
            hashTableBytes *= (1024 * 1024);
        }

        if (hashTableBytes > masterBytes) {
            throw Exception(HERE,
                            "invalid `MasterTotalMemory' and/or "
                            "`HashTableMemory' options - HashTableMemory "
                            "cannot exceed MasterTotalMemory!");
        }

        uint64_t logBytes = masterBytes - hashTableBytes;
        uint64_t numSegments = logBytes / Segment::SEGMENT_SIZE;
        if (numSegments < 1) {
            throw Exception(HERE,
                            "invalid `MasterTotalMemory' and/or "
                            "`HashTableMemory' options - insufficient memory "
                            "left for the log!");
        }

        uint64_t numHashTableLines =
            hashTableBytes / HashTable<LogEntryHandle>::bytesPerCacheLine();
        if (numHashTableLines < 1) {
            throw Exception(HERE,
                            "invalid `MasterTotalMemory' and/or "
                            "`HashTableMemory' options - insufficient memory "
                            "left for the hash table!");
        }

        RAMCLOUD_LOG(NOTICE,
                     "Master to allocate %lu bytes total, %lu of which for the "
                     "hash table", masterBytes, hashTableBytes);
        RAMCLOUD_LOG(NOTICE, "Master will have %lu segments and %lu lines in "
                     "the hash table", numSegments, numHashTableLines);

        master.logBytes = logBytes;
        master.hashTableBytes = hashTableBytes;
    }
};

} // namespace RAMCloud

#endif
