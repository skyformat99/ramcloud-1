/* Copyright (c) 2011 Stanford University
 * Copyright (c) 2011 Facebook
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

// This program runs a RAMCloud client along with a collection of benchmarks
// for measuring the performance of a RAMCloud cluster.  This file works in
// conjunction with clusterperf.py, which starts up the cluster servers
// along with one or more instances of this program. This file contains the
// low-level benchmark code.
//
// TO ADD A NEW BENCHMARK:
// 1. Decide on a symbolic name for the new test.
// 2. Write the function that implements the benchmark.  It goes in the
//    section labeled "test functions" below, in alphabetical order.  The
//    name of the function should be the same as the name of the test.
//    Tests generally work in one of two ways:
//    * The first way is to generate one or more individual metrics;
//      "basic" and "readNotFound" are examples of this style.  Be sure
//      to print results in the same way as existing tests, for consistency.
//    * The second style of test is one that generates a graph;  "readLoaded"
//      is an example of this style.  The test should output graph data in
//      gnuplot format (comma-separated values), with comments at the
//      beginning describing the data and including the name of the test
//      that generated it.
//  3. Add an entry for the new test in the "tests" table below; this is
//     used to dispatch to the test.
//  4. Add code for this test to clusterperf.py, following the instructions
//     in that file.

#include <boost/program_options.hpp>
#include <boost/version.hpp>
#include <iostream>
namespace po = boost::program_options;

#include "RamCloud.h"
#include "CycleCounter.h"
#include "Cycles.h"

using namespace RAMCloud;

// Used to invoke RAMCloud operations.
static RamCloud* cluster;

// Total number of clients that will be participating in this test.
static int numClients;

// Index of this client among all of the participating clients (between
// 0 and numClients-1).  Client 0 acts as master to control the overall
// flow of the test; the other clients are slaves that respond to
// commands from the master.
static int clientIndex;

// Value of the "--size" command-line option: used by some tests to
// determine the number of bytes in each object.  -1 means the option
// wasn't specified, so each test should pick an appropriate default.
static int objectSize;

// Value of the "--numTables" command-line option: used by some tests
// to specify the number of tables to create.
static int numTables;

// Identifier for table that is used for test-specific data.
uint32_t dataTable = -1;

// Identifier for table that is used to communicate between the master
// and slaves to coordinate execution of tests.
uint32_t controlTable = -1;

// The locations of objects in controlTable; each of these values is an
// offset relative to the base for a particular client, as computed by
// controlId.

enum Id {
    STATE =  0,                      // Current state of this client.
    COMMAND = 1,                     // Command issued by master for
                                     // this client.
    DOC = 2,                         // Documentation string in master's
                                     // regions; used in log messages.
    METRICS = 3,                     // Statistics returned from slaves
                                     // to masters.
};

#define MAX_METRICS 8

// The following type holds metrics for all the clients.  Each inner vector
// corresponds to one metric and contains a value from each client, indexed
// by clientIndex.
typedef std::vector<std::vector<double>> ClientMetrics;

//----------------------------------------------------------------------
// Utility functions used by the test functions
//----------------------------------------------------------------------

/**
 * Print a performance measurement consisting of a time value.
 *
 * \param name
 *      Symbolic name for the measurement, in the form test.value
 *      where \c test is the name of the test that generated the result,
 *      \c value is a name for the particular measurement.
 * \param seconds
 *      Time measurement, in seconds.
 * \param description
 *      Longer string (but not more than 20-30 chars) with a human-
 *      readable explanation of what the value refers to.
 */
void
printTime(const char* name, double seconds, const char* description)
{
    printf("%-20s ", name);
    if (seconds < 1.0e-06) {
        printf("%5.1f ns   ", 1e09*seconds);
    } else if (seconds < 1.0e-03) {
        printf("%5.1f us   ", 1e06*seconds);
    } else if (seconds < 1.0) {
        printf("%5.1f ms   ", 1e03*seconds);
    } else {
        printf("%5.1f s    ", seconds);
    }
    printf("  %s\n", description);
}

/**
 * Print a performance measurement consisting of a bandwidth.
 *
 * \param name
 *      Symbolic name for the measurement, in the form test.value
 *      where \c test is the name of the test that generated the result,
 *      \c value is a name for the particular measurement.
 * \param bandwidth
 *      Measurement in units of bytes/second.
 * \param description
 *      Longer string (but not more than 20-30 chars) with a human-
 *      readable explanation of what the value refers to.
 */
void
printBandwidth(const char* name, double bandwidth, const char* description)
{
    double gb = 1024.0*1024.0*1024.0;
    double mb = 1024.0*1024.0;
    double kb = 1024.0;
    printf("%-20s ", name);
    if (bandwidth > gb) {
        printf("%5.1f GB/s ", bandwidth/gb);
    } else if (bandwidth > mb) {
        printf("%5.1f MB/s ", bandwidth/mb);
    } else if (bandwidth >kb) {
        printf("%5.1f KB/s ", bandwidth/kb);
    } else {
        printf("%5.1f B/s  ", bandwidth);
    }
    printf("  %s\n", description);
}

/**
 * Print a performance measurement consisting of a rate.
 *
 * \param name
 *      Symbolic name for the measurement, in the form test.value
 *      where \c test is the name of the test that generated the result,
 *      \c value is a name for the particular measurement.
 * \param value
 *      Measurement in units 1/second.
 * \param description
 *      Longer string (but not more than 20-30 chars) with a human-
 *      readable explanation of what the value refers to.
 */
void
printRate(const char* name, double value, const char* description)
{
    printf("%-20s  ", name);
    if (value > 1e09) {
        printf("%5.1f G/s  ", value/1e09);
    } else if (value > 1e06) {
        printf("%5.1f M/s  ", value/1e06);
    } else if (value > 1e03) {
        printf("%5.1f K/s  ", value/1e03);
    } else {
        printf("%5.1f /s   ", value);
    }
    printf("  %s\n", description);
}

/**
 * Print a performance measurement consisting of a percentage.
 *
 * \param name
 *      Symbolic name for the measurement, in the form test.value
 *      where \c test is the name of the test that generated the result,
 *      \c value is a name for the particular measurement.
 * \param value
 *      Measurement in units of %.
 * \param description
 *      Longer string (but not more than 20-30 chars) with a human-
 *      readable explanation of what the value refers to.
 */
void
printPercent(const char* name, double value, const char* description)
{
    printf("%-20s    %.1f %%      %s\n", name, value, description);
}

/**
 * Time how long it takes to read a particular object repeatedly.
 *
 * \param tableId
 *      Table containing the object.
 * \param objectId
 *      Identifier of the object within its table.
 * \param ms
 *      Read the object repeatedly until this many total ms have
 *      elapsed.
 * \param value
 *      The contents of the object will be stored here, in case
 *      the caller wants to examine them.
 *
 * \return
 *      The average time to read the object, in seconds.
 */
double
timeRead(uint32_t tableId, uint64_t objectId, double ms, Buffer& value)
{
    uint64_t runCycles = Cycles::fromSeconds(ms/1e03);

    // Read the value once just to warm up all the caches everywhere.
    cluster->read(tableId, objectId, &value);

    uint64_t start = Cycles::rdtsc();
    uint64_t elapsed;
    int count = 0;
    while (true) {
        for (int i = 0; i < 10; i++) {
            cluster->read(tableId, objectId, &value);
        }
        count += 10;
        elapsed = Cycles::rdtsc() - start;
        if (elapsed >= runCycles)
            break;
    }
    return Cycles::toSeconds(elapsed)/count;
}

/**
 * Time how long it takes to write a particular object repeatedly.
 *
 * \param tableId
 *      Table containing the object.
 * \param objectId
 *      Identifier of the object within its table.
 * \param value
 *      Pointer to first byte of contents to write into the object.
 * \param length
 *      Size of data at \c value.
 * \param ms
 *      Write the object repeatedly until this many total ms have
 *      elapsed.
 *
 * \return
 *      The average time to write the object, in seconds.
 */
double
timeWrite(uint32_t tableId, uint64_t objectId, const void* value,
        uint32_t length, double ms)
{
    uint64_t runCycles = Cycles::fromSeconds(ms/1e03);

    // Write the value once just to warm up all the caches everywhere.
    cluster->write(tableId, objectId, value, length);

    uint64_t start = Cycles::rdtsc();
    uint64_t elapsed;
    int count = 0;
    while (true) {
        for (int i = 0; i < 10; i++) {
            cluster->write(tableId, objectId, value, length);
        }
        count += 10;
        elapsed = Cycles::rdtsc() - start;
        if (elapsed >= runCycles)
            break;
    }
    return Cycles::toSeconds(elapsed)/count;
}

/**
 * Fill a buffer with an ASCII value that can be checked later to ensure
 * that no data has been lost or corrupted.  A particular tableId and
 * objectId are incorporated into the value (under the assumption that
 * the value will be stored in that object), so that values stored in
 * different objects will be detectably different.
 *
 * \param buffer
 *      Buffer to fill; any existing contents will be discarded.
 * \param size
 *      Number of bytes of data to place in the buffer.
 * \param tableId
 *      This table identifier will be reflected in the value placed in the
 *      buffer.
 * \param objectId
 *      This object identifier will be reflected in the value placed in the
 *      buffer.
 */
void
fillBuffer(Buffer& buffer, uint32_t size, uint32_t tableId, uint64_t objectId)
{
    char chunk[51];
    buffer.reset();
    uint32_t bytesLeft = size;
    int position = 0;
    while (bytesLeft > 0) {
        // Write enough data to completely fill the chunk buffer, then
        // ignore the terminating NULL character that snprintf puts at
        // the end.
        snprintf(chunk, sizeof(chunk),
            "| %d: tableId 0x%x, objectId 0x%lx %s", position, tableId,
            objectId, "0123456789012345678901234567890123456789");
        uint32_t chunkLength = sizeof(chunk) - 1;
        if (chunkLength > bytesLeft) {
            chunkLength = bytesLeft;
        }
        memcpy(new(&buffer, APPEND) char[chunkLength], chunk, chunkLength);
        bytesLeft -= chunkLength;
        position += chunkLength;
    }
}

/**
 * Check the contents of a buffer to ensure that it contains the same data
 * generated previously by fillBuffer.  Generate a log message if a
 * problem is found.
 *
 * \param buffer
 *      Buffer whose contents are to be checked.
 * \param expectedLength
 *      The buffer should contain this many bytes.
 * \param tableId
 *      This table identifier should be reflected in the buffer's data.
 * \param objectId
 *      This object identifier should be reflected in the buffer's data.
 *
 * \return
 *      True means the buffer has the "expected" contents; false means
 *      there was an error.
 */
bool
checkBuffer(Buffer& buffer, uint32_t expectedLength, uint32_t tableId,
        uint64_t objectId)
{
    uint32_t length = buffer.getTotalLength();
    if (length != expectedLength) {
        RAMCLOUD_LOG(ERROR, "corrupted data: expected %u bytes, "
                "found %u bytes", expectedLength, length);
        return false;
    }
    Buffer comparison;
    fillBuffer(comparison, expectedLength, tableId, objectId);
    for (uint32_t i = 0; i < expectedLength; i++) {
        char c1 = *buffer.getOffset<char>(i);
        char c2 = *comparison.getOffset<char>(i);
        if (c1 != c2) {
            int start = i - 10;
            const char* prefix = "...";
            const char* suffix = "...";
            if (start <= 0) {
                start = 0;
                prefix = "";
            }
            uint32_t length = 20;
            if (start+length >= expectedLength) {
                length = expectedLength - start;
                suffix = "";
            }
            RAMCLOUD_LOG(ERROR, "corrupted data: expected '%c', got '%c' "
                    "(\"%s%.*s%s\" vs \"%s%.*s%s\")", c2, c1, prefix, length,
                    static_cast<const char*>(comparison.getRange(start,
                    length)), suffix, prefix, length,
                    static_cast<const char*>(buffer.getRange(start,
                    length)), suffix);
            return false;
        }
    }
    return true;
}

/**
 * Compute the objectId for a particular control value in a particular client.
 *
 * \param client
 *      Index of the desired client.
 * \param id
 *      Control word for the particular client.
 */
uint64_t
objectId(int client, Id id)
{
    return (client << 8) + id;
}

/**
 * Slaves invoke this function to indicate their current state.
 *
 * \param state
 *      A string identifying what the slave is doing now, such as "idle".
 */
void
setSlaveState(const char* state)
{
    cluster->write(controlTable, objectId(clientIndex, STATE), state);
}

/**
 * Read the value of an object and place it in a buffer as a null-terminated
 * string.
 *
 * \param tableId
 *      Identifier of the table containing the object.
 * \param objectId
 *      Identifier of the object within the table.
 * \param value
 *      Buffer in which to store the object's value.
 * \param size
 *      Size of buffer.
 *
 * \return
 *      The return value is a pointer to buffer, which contains the contents
 *      of the specified object, null-terminated and truncated if needed to
 *      make it fit in the buffer.
 */
char*
readObject(uint32_t tableId, uint64_t objectId, char* value, uint32_t size)
{
    Buffer buffer;
    cluster->read(tableId, objectId, &buffer);
    uint32_t actual = buffer.getTotalLength();
    if (size <= actual) {
        actual = size - 1;
    }
    buffer.copy(0, size, value);
    value[actual] = 0;
    return value;
}

/**
 * A slave invokes this function to wait for the master to issue it a
 * command other than "idle"; the string value of the command is returned.
 *
 * \param buffer
 *      Buffer in which to store the state.
 * \param size
 *      Size of buffer.
 * 
 * \return
 *      The return value is a pointer to a buffer, which now holds the
 *      command.
 */
const char*
getCommand(char* buffer, uint32_t size)
{
    while (true) {
        try {
            readObject(controlTable, objectId(clientIndex, COMMAND),
                    buffer, size);
            if (strcmp(buffer, "idle") != 0) {
                // Delete the command value so we don't process the same
                // command twice.
                cluster->remove(controlTable, objectId(clientIndex, COMMAND));
                return buffer;
            }
        }
        catch (TableDoesntExistException& e) {
        }
        catch (ObjectDoesntExistException& e) {
        }
        usleep(10000);
    }
}

/**
 * Wait for a particular object to come into existence and, optionally,
 * for it to take on a particular value.  Give up if the object doesn't
 * reach the desired state within a short time period.
 *
 * \param tableId
 *      Identifier of the table containing the object.
 * \param objectId
 *      Identifier of the object within its table.
 * \param desired
 *      If non-null, specifies a string value; this function won't
 *      return until the object's value matches the string.
 * \param value
 *      The actual value of the object is returned here.
 * \param timeout
 *      Seconds to wait before giving up and throwing an Exception.
 */
void
waitForObject(uint32_t tableId, uint64_t objectId, const char* desired,
        Buffer& value, double timeout = 1.0)
{
    uint64_t start = Cycles::rdtsc();
    size_t length = desired ? strlen(desired) : -1;
    while (true) {
        try {
            cluster->read(tableId, objectId, &value);
            if (desired == NULL) {
                return;
            }
            const char *actual = value.getStart<char>();
            if ((length == value.getTotalLength()) &&
                    (memcmp(actual, desired, length) == 0)) {
                return;
            }
            double elapsed = Cycles::toSeconds(Cycles::rdtsc() - start);
            if (elapsed > timeout) {
                // Slave is taking too long; time out.
                throw Exception(HERE, format(
                        "Object <%u, %lu> didn't reach desired state '%s' "
                        "(actual: '%.*s')",
                        tableId, objectId, desired, downCast<int>(length),
                        actual));
                exit(1);
            }
        }
        catch (TableDoesntExistException& e) {
        }
        catch (ObjectDoesntExistException& e) {
        }
    }
}

/**
 * The master invokes this function to wait for a slave to respond
 * to a command and enter a particular state.  Give up if the slave
 * doesn't enter the desired state within a short time period.
 *
 * \param slave
 *      Index of the slave (1 corresponds to the first slave).
 * \param state
 *      A string identifying the desired state for the slave.
 * \param timeout
 *      Seconds to wait before giving up and throwing an Exception.
 */
void
waitSlave(int slave, const char* state, double timeout = 1.0)
{
    Buffer value;
    waitForObject(controlTable, objectId(slave, STATE), state, value, timeout);
}

/**
 * Issue a command to one or more slaves and wait for them to receive
 * the command.
 *
 * \param command
 *      A string identifying what the slave should do next.  If NULL
 *      then no command is sent; we just wait for the slaves to reach
 *      the given state.
 * \param state
 *      The state that each slave will enter once it has received the
 *      command.  NULL means don't wait for the slaves to receive the
 *      command.
 * \param firstSlave
 *      Index of the first slave to interact with.
 * \param numSlaves
 *      Total number of slaves to command.
 */
void
sendCommand(const char* command, const char* state, int firstSlave,
        int numSlaves = 1)
{
    if (command != NULL) {
        for (int i = 0; i < numSlaves; i++) {
            cluster->write(controlTable, objectId(firstSlave+i, COMMAND),
                    command);
        }
    }
    if (state != NULL) {
        for (int i = 0; i < numSlaves; i++) {
            waitSlave(firstSlave+i, state);
        }
    }
}

/**
 * Create one or more tables, each on a different master, and create one
 * object in each table.
 *
 * \param numTables
 *      How many tables to create.
 * \param objectSize
 *      Number of bytes in the object to create each table.
 * \param objectId
 *      Identifier to use for the created object in each table.
 *
 */
int*
createTables(int numTables, int objectSize, int objectId = 0)
{
    int* tableIds = new int[numTables];

    // Create the tables in backwards order to reduce possible correlations
    // between clients, tables, and servers (if we have 60 clients and 60
    // servers, with clients and servers colocated and client i accessing
    // table i, we wouldn't want each client reading a table from the
    // server on the same machine).
    for (int i = numTables-1; i >= 0;  i--) {
        char tableName[20];
        snprintf(tableName, sizeof(tableName), "table%d", i);
        cluster->createTable(tableName);
        tableIds[i] = cluster->openTable(tableName);
        Buffer data;
        fillBuffer(data, objectSize, tableIds[i], objectId);
        cluster->write(tableIds[i], objectId, data.getRange(0, objectSize),
                objectSize);
    }
    return tableIds;
}

/**
 * Slaves invoke this method to return one or more performance measurements
 * back to the master.
 *
 * \param m0
 *      A performance measurement such as latency or bandwidth.  The precise
 *      meaning is defined by each individual test, and most tests only use
 *      a subset of the possible metrics.
 * \param m1
 *      Another performance measurement.
 * \param m2
 *      Another performance measurement.
 * \param m3
 *      Another performance measurement.
 * \param m4
 *      Another performance measurement.
 * \param m5
 *      Another performance measurement.
 * \param m6
 *      Another performance measurement.
 * \param m7
 *      Another performance measurement.
 */
void
sendMetrics(double m0, double m1 = 0.0, double m2 = 0.0, double m3 = 0.0,
        double m4 = 0.0, double m5 = 0.0, double m6 = 0.0, double m7 = 0.0)
{
    double metrics[MAX_METRICS];
    metrics[0] = m0;
    metrics[1] = m1;
    metrics[2] = m2;
    metrics[3] = m3;
    metrics[4] = m4;
    metrics[5] = m5;
    metrics[6] = m6;
    metrics[7] = m7;
    cluster->write(controlTable, objectId(clientIndex, METRICS), metrics,
            sizeof(metrics));
}

/**
 * Masters invoke this method to retrieved performance measurements from
 * slaves.  This method waits for slaves to fill in their metrics, if they
 * haven't already.
 *
 * \param metrics
 *      This vector of vectors is cleared and then filled with the slaves'
 *      performance data.  Each inner vector corresponds to one metric
 *      and contains a value from each of the slaves.
 * \param clientCount
 *      Metrics will be read from this many clients, starting at 0.
 */
void
getMetrics(ClientMetrics& metrics, int clientCount)
{
    // First, reset the result.
    metrics.clear();
    metrics.resize(MAX_METRICS);
    for (int i = 0; i < MAX_METRICS; i++) {
        metrics[i].resize(clientCount);
        for (int j = 0; j < clientCount; j++) {
            metrics[i][j] = 0.0;
        }
    }

    // Iterate over all the slaves to fetch metrics from each.
    for (int client = 0; client < clientCount; client++) {
        Buffer metricsBuffer;
        waitForObject(controlTable, objectId(client, METRICS), NULL,
                metricsBuffer);
        const double* clientMetrics = static_cast<const double*>(
                metricsBuffer.getRange(0,
                MAX_METRICS*sizeof(double)));      // NOLINT
        for (int i = 0; i < MAX_METRICS; i++) {
            metrics[i][client] = clientMetrics[i];
        }
    }
}

/**
 * Return the largest element in a vector.
 *
 * \param data
 *      Input values.
 */
double
max(std::vector<double>& data)
{
    double result = data[0];
    for (int i = downCast<int>(data.size())-1; i > 0; i--) {
        if (data[i] > result)
            result = data[i];
    }
    return result;
}

/**
 * Return the smallest element in a vector.
 *
 * \param data
 *      Input values.
 */
double
min(std::vector<double>& data)
{
    double result = data[0];
    for (int i = downCast<int>(data.size())-1; i > 0; i--) {
        if (data[i] < result)
            result = data[i];
    }
    return result;
}

/**
 * Return the sum of the elements in a vector.
 *
 * \param data
 *      Input values.
 */
double
sum(std::vector<double>& data)
{
    double result = 0.0;
    for (int i = downCast<int>(data.size())-1; i >= 0; i--) {
        result += data[i];
    }
    return result;
}

/**
 * Return the average of the elements in a vector.
 *
 * \param data
 *      Input values.
 */
double
average(std::vector<double>& data)
{
    double result = 0.0;
    int length = downCast<int>(data.size());
    for (int i = length-1; i >= 0; i--) {
        result += data[i];
    }
    return result / length;
}

//----------------------------------------------------------------------
// Test functions start here
//----------------------------------------------------------------------

// Basic read and write times for objects of different sizes
void
basic()
{
    if (clientIndex != 0)
        return;
    Buffer input, output;
    int sizes[] = {100, 1000, 10000, 100000, 1000000};
    const char* ids[] = {"100", "1K", "10K", "100K", "1M"};
    char name[50], description[50];

    for (int i = 0; i < 5; i++) {
        int size = sizes[i];
        fillBuffer(input, size, dataTable, 44);
        cluster->write(dataTable, 44, input.getRange(0, size), size);
        Buffer output;
        double t = timeRead(dataTable, 44, 100, output);
        checkBuffer(output, size, dataTable, 44);

        snprintf(name, sizeof(name), "basic.read%s", ids[i]);
        snprintf(description, sizeof(description), "read single %sB object",
                ids[i]);
        printTime(name, t, description);
        snprintf(name, sizeof(name), "basic.readBw%s", ids[i]);
        snprintf(description, sizeof(description),
                "bandwidth reading %sB object", ids[i]);
        printBandwidth(name, size/t, description);
    }

    for (int i = 0; i < 5; i++) {
        int size = sizes[i];
        fillBuffer(input, size, dataTable, 44);
        cluster->write(dataTable, 44, input.getRange(0, size), size);
        Buffer output;
        double t = timeWrite(dataTable, 44, input.getRange(0, size),
                size, 100);

        // Make sure the object was properly written.
        cluster->read(dataTable, 44, &output);
        checkBuffer(output, size, dataTable, 44);

        snprintf(name, sizeof(name), "basic.write%s", ids[i]);
        snprintf(description, sizeof(description),
                "write single %sB object", ids[i]);
        printTime(name, t, description);
        snprintf(name, sizeof(name), "basic.writeBw%s", ids[i]);
        snprintf(description, sizeof(description),
                "bandwidth writing %sB object", ids[i]);
        printBandwidth(name, size/t, description);
    }
}

// Measure the time to broadcast a short value from a master to multiple slaves
// using RAMCloud objects.  This benchmark is also useful as a mechanism for
// exercising the master-slave communication mechanisms.
void
broadcast()
{
    if (clientIndex > 0) {
        while (true) {
            char command[20];
            char message[200];
            getCommand(command, sizeof(command));
            if (strcmp(command, "read") == 0) {
                setSlaveState("waiting");
                // Wait for a non-empty DOC string to appear.
                while (true) {
                    readObject(controlTable, objectId(0, DOC), message,
                            sizeof(message));
                    if (message[0] != 0) {
                        break;
                    }
                }
                setSlaveState(message);
            } else if (strcmp(command, "done") == 0) {
                setSlaveState("done");
                RAMCLOUD_LOG(NOTICE, "finished with %s", message);
                return;
            } else {
                RAMCLOUD_LOG(ERROR, "unknown command %s", command);
                return;
            }
        }
    }

    // RAMCLOUD_LOG(NOTICE, "master starting");
    uint64_t totalTime = 0;
    int count = 100;
    for (int i = 0; i < count; i++) {
        char message[30];
        snprintf(message, sizeof(message), "message %d", i);
        cluster->write(controlTable, objectId(clientIndex, DOC), "");
        sendCommand("read", "waiting", 1, numClients-1);
        uint64_t start = Cycles::rdtsc();
        cluster->write(controlTable, objectId(clientIndex, DOC),
                message);
        for (int slave = 1; slave < numClients; slave++) {
            waitSlave(slave, message);
        }
        uint64_t thisRun = Cycles::rdtsc() - start;
        totalTime += thisRun;
    }
    sendCommand("done", "done", 1, numClients-1);
    char description[50];
    snprintf(description, sizeof(description),
            "broadcast message to %d slaves", numClients-1);
    printTime("broadcast", Cycles::toSeconds(totalTime)/count, description);
}

// This benchmark measures overall network bandwidth using many clients, each
// reading repeatedly a single large object on a different server.  The goal
// is to stress the internal network switching fabric without overloading any
// particular client or server.
void
netBandwidth()
{
    int objectId = 99;

    // Duration of the test, in ms.
    int ms = 100;

    if (clientIndex > 0) {
        // Slaves execute the following code.  First, wait for the master
        // to set everything up, then open the table we will use.
        char command[20];
        getCommand(command, sizeof(command));
        char tableName[20];
        snprintf(tableName, sizeof(tableName), "table%d", clientIndex);
        int tableId = cluster->openTable(tableName);
        RAMCLOUD_LOG(NOTICE, "Client %d reading from table %d", clientIndex,
                tableId);
        setSlaveState("running");

        // Read a value from the table repeatedly, and compute bandwidth.
        Buffer value;
        double latency = timeRead(tableId, objectId, ms, value);
        double bandwidth = value.getTotalLength()/latency;
        sendMetrics(bandwidth);
        setSlaveState("done");
        RAMCLOUD_LOG(NOTICE, "Bandwidth (%u-byte object): %.1f MB/sec",
                value.getTotalLength(), bandwidth/(1024*1024));
        return;
    }

    // The master executes the code below.  First, create a table for each
    // slave, with a single object.

    int size = objectSize;
    if (size < 0)
        size = 1024*1024;
    int* tableIds = createTables(numClients, objectSize, objectId);

    // Start all the slaves running, and read our own local object.
    sendCommand("run", "running", 1, numClients-1);
    RAMCLOUD_LOG(DEBUG, "Master reading from table %d", tableIds[0]);
    Buffer value;
    double latency = timeRead(tableIds[0], objectId, 100, value);
    double bandwidth = value.getTotalLength()/latency;
    sendMetrics(bandwidth);

    // Collect statistics.
    ClientMetrics metrics;
    getMetrics(metrics, numClients);
    RAMCLOUD_LOG(DEBUG, "Bandwidth (%u-byte object): %.1f MB/sec",
            value.getTotalLength(), bandwidth/(1024*1024));

    printBandwidth("netBandwidth", sum(metrics[0]),
            "many clients reading from different servers");
    printBandwidth("netBandwidth.max", max(metrics[0]),
            "fastest client");
    printBandwidth("netBandwidth.min", min(metrics[0]),
            "slowest client");
}

// Each client reads a single object from each master.  Good for
// testing that each host in the cluster can send/receive RPCs
// from every other host.
void
readAllToAll()
{
    if (clientIndex > 0) {
        char command[20];
        do {
            getCommand(command, sizeof(command));
            usleep(10 * 1000);
        } while (strcmp(command, "run") != 0);
        setSlaveState("running");
        std::cout << "Slave id " << clientIndex
                  << " reading from all masters" << std::endl;

        for (int tableNum = 0; tableNum < numTables; ++tableNum) {
            string tableName = format("table%d", tableNum);
            try {
                int tableId = cluster->openTable(tableName.c_str());

                Buffer result;
                uint64_t startCycles = Cycles::rdtsc();
                RamCloud::Read read(*cluster, tableId, 0, &result);
                while (!read.isReady()) {
                    Context::get().dispatch->poll();
                    double secsWaiting =
                        Cycles::toSeconds(Cycles::rdtsc() - startCycles);
                    if (secsWaiting > 1.0) {
                        RAMCLOUD_LOG(ERROR,
                                    "Client %d couldn't read from table %s",
                                    clientIndex, tableName.c_str());
                        read.cancel();
                        continue;
                    }
                }
                read();
            } catch (ClientException& e) {
                RAMCLOUD_LOG(ERROR,
                    "Client %d got exception reading from table %s: %s",
                    clientIndex, tableName.c_str(), e.what());
            } catch (...) {
                RAMCLOUD_LOG(ERROR,
                    "Client %d got unknown exception reading from table %s",
                    clientIndex, tableName.c_str());
            }
        }
        setSlaveState("done");
        return;
    }

    int size = objectSize;
    if (size < 0)
        size = 100;
    int* tableIds = createTables(numTables, size);

    std::cout << "Master client reading from all masters" << std::endl;
    for (int i = 0; i < numTables; ++i) {
        int tableId = tableIds[i];
        Buffer result;
        uint64_t startCycles = Cycles::rdtsc();
        RamCloud::Read read(*cluster, tableId, 0, &result);
        while (!read.isReady()) {
            Context::get().dispatch->poll();
            if (Cycles::toSeconds(Cycles::rdtsc() - startCycles) > 1.0) {
                RAMCLOUD_LOG(ERROR,
                            "Master client %d couldn't read from tableId %d",
                            clientIndex, tableId);
                return;
            }
        }
        read();
    }

    for (int slaveIndex = 1; slaveIndex < numClients; ++slaveIndex) {
        sendCommand("run", "running", slaveIndex);
        // Give extra time if clients have to contact a lot of masters.
        waitSlave(slaveIndex, "done", 1.0 + 0.1 * numTables);
    }

    delete[] tableIds;
}

// This benchmark measures the latency and server throughput for reads
// when several clients are simultaneously reading the same object.
void
readLoaded()
{
    if (clientIndex > 0) {
        // Slaves execute the following code, which creates load by
        // repeatedly reading a particular object.
        while (true) {
            char command[20];
            char doc[200];
            getCommand(command, sizeof(command));
            if (strcmp(command, "run") == 0) {
                readObject(controlTable, objectId(0, DOC), doc, sizeof(doc));
                setSlaveState("running");

                // Although the main purpose here is to generate load, we
                // also measure performance, which can be checked to ensure
                // that all clients are seeing roughly the same performance.
                // Only measure performance when the size of the object is
                // nonzero (this indicates that all clients are active)
                uint64_t start = 0;
                Buffer buffer;
                int count = 0;
                int size = 0;
                while (true) {
                    cluster->read(dataTable, 111, &buffer);
                    int currentSize = buffer.getTotalLength();
                    if (currentSize != 0) {
                        if (start == 0) {
                            start = Cycles::rdtsc();
                            size = currentSize;
                        }
                        count++;
                    } else {
                        if (start != 0)
                            break;
                    }
                }
                RAMCLOUD_LOG(NOTICE, "Average latency (size %d): %.1fus (%s)",
                        size, Cycles::toSeconds(Cycles::rdtsc() - start)
                        *1e06/count, doc);
                setSlaveState("idle");
            } else if (strcmp(command, "done") == 0) {
                setSlaveState("done");
                return;
            } else {
                RAMCLOUD_LOG(ERROR, "unknown command %s", command);
                return;
            }
        }
    }

    // The master executes the following code, which starts up zero or more
    // slaves to generate load, then times the performance of reading.
    int size = objectSize;
    if (size < 0)
        size = 100;
    printf("# RAMCloud read performance as a function of load (1 or more\n");
    printf("# clients all reading a single %d-byte object repeatedly).\n",
            size);
    printf("# Generated by 'clusterperf.py readLoaded'\n");
    printf("#\n");
    printf("# numClients  readLatency(us)  throughput(total kreads/sec)\n");
    printf("#----------------------------------------------------------\n");
    for (int numSlaves = 0; numSlaves < numClients; numSlaves++) {
        char message[100];
        Buffer input, output;
        snprintf(message, sizeof(message), "%d active clients", numSlaves+1);
        cluster->write(controlTable, objectId(0, DOC), message);
        cluster->write(dataTable, 111, "");
        sendCommand("run", "running", 1, numSlaves);
        fillBuffer(input, size, dataTable, 111);
        cluster->write(dataTable, 111, input.getRange(0, size), size);
        double t = timeRead(dataTable, 111, 100, output);
        cluster->write(dataTable, 111, "");
        checkBuffer(output, size, dataTable, 111);
        printf("%5d     %10.1f          %8.0f\n", numSlaves+1, t*1e06,
                (numSlaves+1)/(1e03*t));
        sendCommand(NULL, "idle", 1, numSlaves);
    }
    sendCommand("done", "done", 1, numClients-1);
}

// Read an object that doesn't exist. This excercises some exception paths that
// are supposed to be fast. This comes up, for example, in workloads in which a
// RAMCloud is used as a cache with frequent cache misses.
void
readNotFound()
{
    if (clientIndex != 0)
        return;

    uint64_t runCycles = Cycles::fromSeconds(.1);

    // Similar to timeRead but catches the exception
    uint64_t start = Cycles::rdtsc();
    uint64_t elapsed;
    int count = 0;
    while (true) {
        for (int i = 0; i < 10; i++) {
            Buffer output;
            try {
                cluster->read(dataTable, 55, &output);
            } catch (const ObjectDoesntExistException& e) {
                continue;
            }
            throw Exception(HERE, "Object exists?");
        }
        count += 10;
        elapsed = Cycles::rdtsc() - start;
        if (elapsed >= runCycles)
            break;
    }
    double t = Cycles::toSeconds(elapsed)/count;

    printTime("readNotFound", t, "read object that doesn't exist");
}

/**
 * This method contains the core of the "readRandom" test; it is
 * shared by the master and slaves.
 *
 * \param tableIds
 *      Array of numTables identifiers for the tables available for
 *      the test.
 * \param docString
 *      Information provided by the master about this run; used
 *      in log messages.
 */
void readRandomCommon(int *tableIds, char *docString)
{
    // Duration of test.
    double ms = 100;
    uint64_t startTime = Cycles::rdtsc();
    uint64_t endTime = startTime + Cycles::fromSeconds(ms/1e03);
    uint64_t slowTicks = Cycles::fromSeconds(10e-06);
    uint64_t readStart, readEnd;
    uint64_t maxLatency = 0;
    int count = 0;
    int slowReads = 0;

    // Each iteration through this loop issues one read operation to a
    // randomly-selected table.
    while (true) {
        int tableId = tableIds[downCast<int>(generateRandom() % numTables)];
        readStart = Cycles::rdtsc();
        Buffer value;
        cluster->read(tableId, 0, &value);
        readEnd = Cycles::rdtsc();
        count++;
        uint64_t latency = readEnd - readStart;

        // When computing the slowest read, skip the first reads so that
        // everything has a chance to get fully warmed up.
        if ((latency > maxLatency) && (count > 100))
            maxLatency = latency;
        if (latency > slowTicks)
            slowReads++;
        if (readEnd > endTime)
            break;
    }
    double thruput = count/Cycles::toSeconds(readEnd - startTime);
    double slowPercent = 100.0 * slowReads / count;
    sendMetrics(thruput, Cycles::toSeconds(maxLatency), slowPercent);
    if (clientIndex != 0) {
        RAMCLOUD_LOG(NOTICE,
                "%s: throughput: %.1f reads/sec., max latency: %.1fus, "
                "reads > 20us: %.1f%%", docString,
                thruput, Cycles::toSeconds(maxLatency)*1e06, slowPercent);
    }
}

// In this test all of the clients repeatedly read objects from a collection
// of tables on different servers.  For each read a client chooses a table
// at random.
void
readRandom()
{
    int *tableIds = NULL;

    if (clientIndex > 0) {
        // This is a slave: execute commands coming from the master.
        while (true) {
            char command[20];
            char doc[200];
            getCommand(command, sizeof(command));
            if (strcmp(command, "run") == 0) {
                if (tableIds == NULL) {
                    // Open all the tables.
                    tableIds = new int[numTables];
                    for (int i = 0; i < numTables; i++) {
                        char tableName[20];
                        snprintf(tableName, sizeof(tableName), "table%d", i);
                        tableIds[i] = cluster->openTable(tableName);
                    }
                }
                readObject(controlTable, objectId(0, DOC), doc, sizeof(doc));
                setSlaveState("running");
                readRandomCommon(tableIds, doc);
                setSlaveState("idle");
            } else if (strcmp(command, "done") == 0) {
                setSlaveState("done");
                return;
            } else {
                RAMCLOUD_LOG(ERROR, "unknown command %s", command);
                return;
            }
        }
    }

    // This is the master: first, create the tables.
    int size = objectSize;
    if (size < 0)
        size = 100;
    tableIds = createTables(numTables, size);

    // Vary the number of clients and repeat the test for each number.
    printf("# RAMCloud read performance when 1 or more clients read\n");
    printf("# %d-byte objects chosen at random from %d servers.\n",
            size, numTables);
    printf("# Generated by 'clusterperf.py readRandom'\n");
    printf("#\n");
    printf("# numClients  throughput(total kreads/sec)  slowest(ms)  "
                "reads > 10us\n");
    printf("#--------------------------------------------------------"
                "------------\n");
    fflush(stdout);
    for (int numActive = 1; numActive <= numClients; numActive++) {
        char doc[100];
        snprintf(doc, sizeof(doc), "%d active clients", numActive);
        cluster->write(controlTable, objectId(0, DOC), doc);
        sendCommand("run", "running", 1, numActive-1);
        readRandomCommon(tableIds, doc);
        sendCommand(NULL, "idle", 1, numActive-1);
        ClientMetrics metrics;
        getMetrics(metrics, numActive);
        printf("%3d               %6.0f                    %6.2f"
                "          %.1f%%\n",
                numActive, sum(metrics[0])/1e03, max(metrics[1])*1e03,
                sum(metrics[2])/numActive);
        fflush(stdout);
    }
    sendCommand("done", "done", 1, numClients-1);
}

// This benchmark measures the latency and server throughput for write
// when some data is written asynchronously and then some smaller value
// is written synchronously.
void
writeAsyncSync()
{
    if (clientIndex > 0)
        return;

    const uint32_t count = 100;
    const uint32_t syncObjectSize = 100;
    const uint32_t asyncObjectSizes[] = { 100, 1000, 10000, 100000, 1000000 };
    const uint32_t arrayElts = sizeof(asyncObjectSizes) /
                               sizeof(asyncObjectSizes[0]);

    uint32_t maxSize = syncObjectSize;
    for (uint32_t j = 0; j < arrayElts; ++j)
        maxSize = std::max(maxSize, asyncObjectSizes[j]);

    char* garbage = new char[maxSize];
    // prime
    cluster->write(dataTable, 111, &garbage[0], syncObjectSize);
    cluster->write(dataTable, 111, &garbage[0], syncObjectSize);

    printf("# RAMCloud %u B write performance during interleaved\n",
           syncObjectSize);
    printf("# asynchronous writes of various sizes\n");
    printf("# Generated by 'clusterperf.py writeAsyncSync'\n#\n");
    printf("# firstWriteIsSync firstObjectSize firstWriteLatency(us) "
            "syncWriteLatency(us)\n");
    printf("#--------------------------------------------------------"
            "--------------------\n");
    for (int sync = 0; sync < 2; ++sync) {
        for (uint32_t j = 0; j < arrayElts; ++j) {
            const uint32_t asyncObjectSize = asyncObjectSizes[j];
            uint64_t asyncTicks = 0;
            uint64_t syncTicks = 0;
            for (uint32_t i = 0; i < count; ++i) {
                {
                    CycleCounter<> _(&asyncTicks);
                    cluster->write(dataTable, 111, &garbage[0], asyncObjectSize,
                                   NULL, NULL, !sync);
                }
                {
                    CycleCounter<> _(&syncTicks);
                    cluster->write(dataTable, 111, &garbage[0], syncObjectSize);
                }
            }
            printf("%18d %15u %21.1f %20.1f\n", sync, asyncObjectSize,
                   Cycles::toSeconds(asyncTicks) * 1e6 / count,
                   Cycles::toSeconds(syncTicks) * 1e6 / count);
        }
    }

    delete garbage;
}

// The following struct and table define each performance test in terms of
// a string name and a function that implements the test.
struct TestInfo {
    const char* name;             // Name of the performance test; this is
                                  // what gets typed on the command line to
                                  // run the test.
    void (*func)();               // Function that implements the test.
};
TestInfo tests[] = {
    {"basic", basic},
    {"broadcast", broadcast},
    {"netBandwidth", netBandwidth},
    {"readAllToAll", readAllToAll},
    {"readLoaded", readLoaded},
    {"readNotFound", readNotFound},
    {"readRandom", readRandom},
    {"writeAsyncSync", writeAsyncSync},
};

int
main(int argc, char *argv[])
try
{
    // need external context to set log levels
    Context context(true);
    Context::Guard _(context);

    // Parse command-line options.
    vector<string> testNames;
    string coordinatorLocator, logFile;
    string logLevel("NOTICE");
    po::options_description desc(
            "Usage: ClusterPerf [options] testName testName ...\n\n"
            "Runs one or more benchmarks on a RAMCloud cluster and outputs\n"
            "performance information.  This program is not normally invoked\n"
            "directly; it is invoked by the clusterperf script.\n\n"
            "Allowed options:");
    desc.add_options()
        ("clientIndex", po::value<int>(&clientIndex)->default_value(0),
                "Index of this client (first client is 0)")
        ("coordinator,C", po::value<string>(&coordinatorLocator),
                "Service locator for the cluster coordinator (required)")
        ("logFile", po::value<string>(&logFile),
                "Redirect all output to this file")
        ("logLevel,l", po::value<string>(&logLevel)->default_value("NOTICE"),
                "Print log messages only at this severity level or higher "
                "(ERROR, WARNING, NOTICE, DEBUG)")
        ("help,h", "Print this help message")
        ("numClients", po::value<int>(&numClients)->default_value(1),
                "Total number of clients running")
        ("size,s", po::value<int>(&objectSize)->default_value(-1),
                "Size of objects (in bytes) to use for test")
        ("numTables", po::value<int>(&numTables)->default_value(10),
                "Number of tables to use for test")
        ("testName", po::value<vector<string>>(&testNames),
                "Name(s) of test(s) to run");
    po::positional_options_description desc2;
    desc2.add("testName", -1);
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
            options(desc).positional(desc2).run(), vm);
    po::notify(vm);
    if (logFile.size() != 0) {
        // Redirect both stdout and stderr to the log file.  Don't
        // call logger.setLogFile, since that will not affect printf
        // calls.
        FILE* f = fopen(logFile.c_str(), "w");
        if (f == NULL) {
            RAMCLOUD_LOG(ERROR, "couldn't open log file '%s': %s",
                    logFile.c_str(), strerror(errno));
            exit(1);
        }
        stdout = stderr = f;
    }
    Context::get().logger->setLogLevels(logLevel);
    if (vm.count("help")) {
        std::cout << desc << '\n';
        exit(0);
    }
    if (coordinatorLocator.empty()) {
        RAMCLOUD_LOG(ERROR, "missing required option --coordinator");
        exit(1);
    }

    RamCloud r(context, coordinatorLocator.c_str());
    cluster = &r;
    cluster->createTable("data");
    dataTable = cluster->openTable("data");
    cluster->createTable("control");
    controlTable = cluster->openTable("control");

    if (testNames.size() == 0) {
        // No test names specified; run all tests.
        foreach (TestInfo& info, tests) {
            info.func();
        }
    } else {
        // Run only the tests that were specified on the command line.
        foreach (string& name, testNames) {
            bool foundTest = false;
            foreach (TestInfo& info, tests) {
                if (name.compare(info.name) == 0) {
                    foundTest = true;
                    info.func();
                    break;
                }
            }
            if (!foundTest) {
                printf("No test named '%s'\n", name.c_str());
            }
        }
    }
}
catch (std::exception& e) {
    RAMCLOUD_LOG(ERROR, "%s", e.what());
    exit(1);
}
