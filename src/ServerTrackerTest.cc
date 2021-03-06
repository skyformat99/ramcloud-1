/* Copyright (c) 2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"
#include "TestLog.h"
#include "ServerList.h"
#include "ServerTracker.h"

namespace RAMCloud {

struct CountCallback : public ServerTracker<int>::Callback {
    CountCallback() : callbacksFired() {}
    void trackerChangesEnqueued() { ++callbacksFired; }
    int callbacksFired;
};

class ServerTrackerTest : public ::testing::Test {
  public:
    ServerTrackerTest()
        : callback()
        , sl()
        , tr(sl)
        , trcb(sl, &callback)
    {
    }

    CountCallback callback;
    ServerList sl;
    ServerTracker<int> tr;
    ServerTracker<int> trcb;

    DISALLOW_COPY_AND_ASSIGN(ServerTrackerTest);
};

TEST_F(ServerTrackerTest, constructors) {
    EXPECT_EQ(0U, tr.serverList.size());
    EXPECT_FALSE(tr.changes.areChanges());
    EXPECT_FALSE(tr.eventCallback);
    EXPECT_EQ(static_cast<uint32_t>(-1), tr.lastRemovedIndex);

    EXPECT_EQ(0U, trcb.serverList.size());
    EXPECT_FALSE(trcb.changes.areChanges());
    EXPECT_TRUE(trcb.eventCallback);
    EXPECT_TRUE(&callback == trcb.eventCallback);
    EXPECT_EQ(static_cast<uint32_t>(-1), trcb.lastRemovedIndex);
}

TEST_F(ServerTrackerTest, enqueueChange) {
    EXPECT_EQ(0U, tr.serverList.size());
    EXPECT_EQ(0U, tr.changes.changes.size());
    tr.enqueueChange(ServerDetails(ServerId(2, 0)),
                     ServerChangeEvent::SERVER_ADDED);
    EXPECT_EQ(3U, tr.serverList.size());
    EXPECT_EQ(1U, tr.changes.changes.size());

    trcb.enqueueChange(ServerDetails(ServerId(0, 0)),
                       ServerChangeEvent::SERVER_ADDED);
    EXPECT_EQ(1, callback.callbacksFired);
    trcb.enqueueChange(ServerDetails(ServerId(0, 0)),
                       ServerChangeEvent::SERVER_REMOVED);
    EXPECT_EQ(2, callback.callbacksFired);

    // Ensure nothing was actually added to the lists.
    for (size_t i = 0; i < tr.serverList.size(); i++) {
        EXPECT_FALSE(tr.serverList[i].server.serverId.isValid());
        EXPECT_TRUE(tr.serverList[i].pointer == NULL);
    }
    for (size_t i = 0; i < trcb.serverList.size(); i++) {
        EXPECT_FALSE(trcb.serverList[i].server.serverId.isValid());
        EXPECT_TRUE(trcb.serverList[i].pointer == NULL);
    }
}

TEST_F(ServerTrackerTest, areChanges) {
    EXPECT_FALSE(tr.areChanges());
    tr.enqueueChange(ServerDetails(ServerId(2, 0)),
                     ServerChangeEvent::SERVER_ADDED);
    EXPECT_TRUE(tr.areChanges());
}

static bool
getChangeFilter(string s)
{
    return (s == "getChange");
}

TEST_F(ServerTrackerTest, getChange) {
    TestLog::Enable _(&getChangeFilter);
    ServerDetails server;
    ServerChangeEvent event;

    // Add
    EXPECT_FALSE(tr.getChange(server, event));
    EXPECT_EQ(0U, tr.serverList.size());
    tr.enqueueChange(ServerDetails(ServerId(2, 0), "Prophylaxis",
                     {BACKUP_SERVICE}), ServerChangeEvent::SERVER_ADDED);
    EXPECT_EQ(3U, tr.serverList.size());
    EXPECT_FALSE(tr.serverList[2].server.serverId.isValid());
    EXPECT_TRUE(tr.serverList[2].pointer == NULL);
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_EQ(ServerId(2, 0), server.serverId);
    EXPECT_EQ("Prophylaxis", server.serviceLocator);
    EXPECT_TRUE(server.services.has(BACKUP_SERVICE));
    EXPECT_FALSE(server.services.has(MASTER_SERVICE));
    EXPECT_EQ(ServerChangeEvent::SERVER_ADDED, event);
    EXPECT_FALSE(tr.getChange(server, event));
    EXPECT_EQ(ServerId(2, 0), tr.serverList[2].server.serverId);
    EXPECT_TRUE(tr.serverList[2].pointer == NULL);

    // Remove
    tr[ServerId(2, 0)] = reinterpret_cast<int*>(57);
    tr.enqueueChange(ServerDetails(ServerId(2, 0)),
                     ServerChangeEvent::SERVER_REMOVED);
    EXPECT_EQ(reinterpret_cast<void*>(57), tr[ServerId(2, 0)]);
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_EQ(ServerId(2, 0), server.serverId);
    EXPECT_EQ(ServerChangeEvent::SERVER_REMOVED, event);
    EXPECT_EQ(2U, tr.lastRemovedIndex);
    tr.testing_avoidGetChangeAssertion = true;
    EXPECT_FALSE(tr.getChange(server, event));
    EXPECT_EQ("getChange: User of this ServerTracker did not NULL out previous "
        "pointer for index 2 (ServerId 2)!", TestLog::get());
    EXPECT_FALSE(tr.serverList[2].server.serverId.isValid());
    EXPECT_EQ("", tr.serverList[2].server.serviceLocator);
    EXPECT_EQ(0u, tr.serverList[2].server.services.serialize());
    EXPECT_TRUE(tr.serverList[2].pointer == NULL);
    EXPECT_EQ(static_cast<uint32_t>(-1), tr.lastRemovedIndex);
}

TEST_F(ServerTrackerTest, getRandomServerIdWithService) {
    Context::get().logger->setLogLevels(SILENT_LOG_LEVEL);

    ServerDetails server;
    ServerChangeEvent event;

    EXPECT_FALSE(tr.getRandomServerIdWithService(MASTER_SERVICE).isValid());
    tr.enqueueChange(ServerDetails(ServerId(0, 1), "", {MASTER_SERVICE}),
                     ServerChangeEvent::SERVER_ADDED);
    EXPECT_FALSE(tr.getRandomServerIdWithService(MASTER_SERVICE).isValid());

    EXPECT_TRUE(tr.getChange(server, event));
    for (int i = 0; i < 10; i++) {
        // Ensure asking for a specific service filters properly.
        // Should find one with low order bit set.
        EXPECT_EQ(ServerId(0, 1),
                  tr.getRandomServerIdWithService(MASTER_SERVICE));
        // No host available with this service bit set.
        EXPECT_EQ(ServerId(),
                  tr.getRandomServerIdWithService(BACKUP_SERVICE));
    }

    tr.enqueueChange(ServerDetails(ServerId(1, 1), "", {MASTER_SERVICE}),
                     ServerChangeEvent::SERVER_ADDED);

    EXPECT_TRUE(tr.getChange(server, event));
    bool firstSeen = false;
    bool secondSeen = false;
    for (int i = 0; i < 100; i++) {
        ServerId id = tr.getRandomServerIdWithService(MASTER_SERVICE);
        EXPECT_TRUE(id == ServerId(0, 1) ||
                    id == ServerId(1, 1));
        if (id == ServerId(0, 1)) firstSeen = true;
        if (id == ServerId(1, 1)) secondSeen = true;
    }
    EXPECT_TRUE(firstSeen);
    EXPECT_TRUE(secondSeen);

    // Ensure looping over empty list terminates.
    tr.enqueueChange(ServerDetails(ServerId(0, 1)),
                     ServerChangeEvent::SERVER_REMOVED);
    tr.enqueueChange(ServerDetails(ServerId(1, 1)),
                     ServerChangeEvent::SERVER_REMOVED);
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_FALSE(tr.getRandomServerIdWithService({MASTER_SERVICE}).isValid());
}

TEST_F(ServerTrackerTest, getRandomServerIdWithService_evenDistribution) {
    Context::get().logger->setLogLevels(SILENT_LOG_LEVEL);

    ServerDetails server;
    ServerChangeEvent event;
    tr.enqueueChange(ServerDetails(ServerId(1, 0), "", {BACKUP_SERVICE}),
                     ServerChangeEvent::SERVER_ADDED);
    tr.enqueueChange(ServerDetails(ServerId(2, 0), "", {BACKUP_SERVICE}),
                     ServerChangeEvent::SERVER_ADDED);
    tr.enqueueChange(ServerDetails(ServerId(3, 0), "", {BACKUP_SERVICE}),
                     ServerChangeEvent::SERVER_ADDED);
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_FALSE(tr.getChange(server, event));
    ASSERT_EQ(3u, tr.size());

    std::vector<uint32_t> counts(tr.size(), 0);
    for (int i = 0; i < 10000; ++i) {
        ServerId id =
            tr.getRandomServerIdWithService(BACKUP_SERVICE);
        counts[id.indexNumber() - 1]++;
    }

    // Check to make sure the most-significant digit is what we expect:
    // Each backup should be returned about 1/3 of the time (3333 times).
    foreach (uint32_t count, counts) {
        LOG(ERROR, "%u", count);
        EXPECT_EQ(3u, count / 1000);
    }
}

TEST_F(ServerTrackerTest, getLocator) {
    EXPECT_THROW(tr.getLocator(ServerId(1, 0)), Exception);
    tr.enqueueChange(ServerDetails(ServerId(1, 1), "mock:",
                                         {MASTER_SERVICE}),
                     ServerChangeEvent::SERVER_ADDED);
    ServerDetails server;
    ServerChangeEvent event;
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_THROW(tr.getLocator(ServerId(2, 0)), Exception);
    EXPECT_EQ("mock:", tr.getLocator(ServerId(1, 1)));
}

TEST_F(ServerTrackerTest, getServerDetails) {
    EXPECT_THROW(tr.getLocator(ServerId(1, 0)), Exception);
    ServerDetails details(ServerId(1, 1), "mock:", {MASTER_SERVICE});
    tr.enqueueChange(details, ServerChangeEvent::SERVER_ADDED);
    ServerDetails server;
    ServerChangeEvent event;
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_THROW(tr.getLocator(ServerId(2, 0)), Exception);
    EXPECT_EQ(details.services.serialize(),
              tr.getServerDetails(ServerId(1, 1))->services.serialize());
}

TEST_F(ServerTrackerTest, indexOperator) {
    TestLog::Enable _; // suck up getChange WARNING
    ServerDetails server;
    ServerChangeEvent event;

    EXPECT_THROW(tr[ServerId(0, 0)], Exception);

    tr.enqueueChange(ServerDetails(ServerId(0, 0)),
                     ServerChangeEvent::SERVER_ADDED);
    EXPECT_TRUE(tr.getChange(server, event));
    tr[ServerId(0, 0)] = reinterpret_cast<int*>(45);
    EXPECT_THROW(tr[ServerId(0, 1)], Exception);
    EXPECT_EQ(reinterpret_cast<int*>(45), tr[ServerId(0, 0)]);
    EXPECT_THROW(tr[ServerId(0, 1)], Exception);

    tr.enqueueChange(ServerDetails(ServerId(0, 0)),
                     ServerChangeEvent::SERVER_REMOVED);
    EXPECT_TRUE(tr.getChange(server, event));
    EXPECT_NO_THROW(tr[ServerId(0, 0)]);
    EXPECT_NE(static_cast<int*>(NULL), tr.serverList[0].pointer);
    tr.testing_avoidGetChangeAssertion = true;
    EXPECT_FALSE(tr.getChange(server, event));
    EXPECT_THROW(tr[ServerId(0, 0)], Exception);
    EXPECT_EQ(static_cast<int*>(NULL), tr.serverList[0].pointer);
}

TEST_F(ServerTrackerTest, size) {
    ServerDetails server;
    ServerChangeEvent event;

    EXPECT_EQ(0U, tr.size());
    tr.enqueueChange(ServerDetails(ServerId(0, 0)),
                     ServerChangeEvent::SERVER_ADDED);
    EXPECT_EQ(0U, tr.size());
    tr.getChange(server, event);
    EXPECT_EQ(1U, tr.size());

    tr.enqueueChange(ServerDetails(ServerId(0, 0)),
                     ServerChangeEvent::SERVER_REMOVED);
    EXPECT_EQ(1U, tr.size());
    tr.getChange(server, event);
    EXPECT_EQ(0U, tr.size());
}

TEST_F(ServerTrackerTest, ChangeQueue_addChange) {
    EXPECT_EQ(0U, tr.changes.changes.size());
    tr.changes.addChange(ServerDetails(ServerId(5, 4)),
                         ServerChangeEvent::SERVER_ADDED);
    EXPECT_EQ(1U, tr.changes.changes.size());
    EXPECT_EQ(ServerId(5, 4), tr.changes.changes.front().server.serverId);
    EXPECT_EQ(ServerChangeEvent::SERVER_ADDED,
        tr.changes.changes.front().event);
}

TEST_F(ServerTrackerTest, ChangeQueue_getChange) {
    EXPECT_THROW(tr.changes.getChange(), Exception);

    tr.changes.addChange(ServerDetails(ServerId(5, 4)),
                         ServerChangeEvent::SERVER_ADDED);
    ServerTracker<int>::ServerChange change = tr.changes.getChange();
    EXPECT_EQ(0U, tr.changes.changes.size());
    EXPECT_EQ(ServerId(5, 4), change.server.serverId);
    EXPECT_EQ(ServerChangeEvent::SERVER_ADDED, change.event);
    EXPECT_THROW(tr.changes.getChange(), Exception);
}

TEST_F(ServerTrackerTest, ChangeQueue_areChanges) {

}

}  // namespace RAMCloud
