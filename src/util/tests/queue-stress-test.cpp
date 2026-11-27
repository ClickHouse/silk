#include <silk/util/assert.h>
#include <silk/util/platform.h>
#include <silk/util/queue.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <sched.h>

namespace silk
{

/**
 * Producers and consumers of two queues that share the node pool, all on a few CPUs,
 * so that the OS preempts them at arbitrary instructions and nodes are reused at once.
 */
class QueueStressTest : public ::testing::Test
{
public:
    void SetUp() override;
    void TearDown() override;

protected:
    //
    // Constants.
    //

    /** Number of CPUs the test threads share. */
    static constexpr uint32_t SHARED_CPU_COUNT = 2;

    /** Number of queues that share the node pool. */
    static constexpr uint32_t QUEUE_COUNT = 2;

    /** Number of producer threads; producer i enqueues into queue i % QUEUE_COUNT. */
    static constexpr uint32_t PRODUCER_COUNT = 6;

    /** Number of values each producer enqueues. */
    static constexpr uint32_t VALUES_PER_PRODUCER = 250000;

    //
    // Data structures.
    //

    /** One enqueued value. */
    struct Value
    {
        /** Index of the queue the value is enqueued into. */
        uint32_t queueIndex = 0;

        /** Index of the producer that enqueues the value. */
        uint32_t producerIndex = 0;

        /** Position of the value in its producer's enqueue order. */
        uint32_t sequence = 0;

        /** Number of times a consumer dequeued the value. */
        std::atomic<uint32_t> deliveryCount{};
    };

    /** The queue type under test. */
    using ValueQueue = Queue<Value>;

    /** Violations seen by one consumer. */
    struct ConsumerResult
    {
        /** Values dequeued from a queue other than the one they were enqueued into. */
        uint64_t wrongQueueCount = 0;

        /** Values dequeued after a later value of the same producer. */
        uint64_t orderBreakCount = 0;

        /** Values dequeued more than once. */
        uint64_t duplicateCount = 0;
    };

    //
    // Threads.
    //

    /** Enqueue VALUES_PER_PRODUCER values into queue in sequence order. */
    static void runProducer(ValueQueue * queue, Value * values) noexcept;

    /** Dequeue from queue until producersDone is set and queue is empty; as its only consumer, see its dequeue order. */
    static void
    runConsumer(ValueQueue * queue, uint32_t queueIndex, const std::atomic<bool> * producersDone, ConsumerResult * result) noexcept;

    /** Check one value dequeued from queueIndex against the previous values of its producer. */
    static void checkValue(Value * value, uint32_t queueIndex, int64_t * lastSequences, ConsumerResult * result) noexcept;

    //
    // State.
    //

    /** Affinity of the main thread before SetUp, restored by TearDown. */
    cpu_set_t savedAffinity;
};

void QueueStressTest::SetUp()
{
    QueueBase::initialize();

    int r = sched_getaffinity(0, sizeof(cpu_set_t), &savedAffinity);
    ASSERT_FALSE(r);

    // The test threads inherit the affinity of the main thread.
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    uint32_t cpuCount = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && cpuCount < SHARED_CPU_COUNT; ++cpu)
    {
        if (CPU_ISSET(cpu, &savedAffinity))
        {
            CPU_SET(cpu, &affinity);
            ++cpuCount;
        }
    }

    r = sched_setaffinity(0, sizeof(cpu_set_t), &affinity);
    ASSERT_FALSE(r);
}

void QueueStressTest::TearDown()
{
    int r = sched_setaffinity(0, sizeof(cpu_set_t), &savedAffinity);
    ASSERT_FALSE(r);

    QueueBase::destroy();
}

TEST_F(QueueStressTest, TwoQueuesDeliverInProducerOrder)
{
    ValueQueue queues[QUEUE_COUNT];
    std::vector<Value> values(PRODUCER_COUNT * VALUES_PER_PRODUCER);
    for (uint32_t producerIndex = 0; producerIndex < PRODUCER_COUNT; ++producerIndex)
    {
        for (uint32_t sequence = 0; sequence < VALUES_PER_PRODUCER; ++sequence)
        {
            Value * value = &values[producerIndex * VALUES_PER_PRODUCER + sequence];
            value->queueIndex = producerIndex % QUEUE_COUNT;
            value->producerIndex = producerIndex;
            value->sequence = sequence;
        }
    }

    std::atomic<bool> producersDone{false};
    ConsumerResult results[QUEUE_COUNT];
    std::thread consumers[QUEUE_COUNT];
    for (uint32_t queueIndex = 0; queueIndex < QUEUE_COUNT; ++queueIndex)
    {
        consumers[queueIndex] = std::thread(runConsumer, &queues[queueIndex], queueIndex, &producersDone, &results[queueIndex]);
    }

    std::thread producers[PRODUCER_COUNT];
    for (uint32_t producerIndex = 0; producerIndex < PRODUCER_COUNT; ++producerIndex)
    {
        ValueQueue * queue = &queues[producerIndex % QUEUE_COUNT];
        producers[producerIndex] = std::thread(runProducer, queue, &values[producerIndex * VALUES_PER_PRODUCER]);
    }

    for (std::thread & producer : producers)
    {
        producer.join();
    }

    producersDone.store(true, std::memory_order_release);
    for (std::thread & consumer : consumers)
    {
        consumer.join();
    }

    ConsumerResult total;
    for (const ConsumerResult & result : results)
    {
        total.wrongQueueCount += result.wrongQueueCount;
        total.orderBreakCount += result.orderBreakCount;
        total.duplicateCount += result.duplicateCount;
    }

    uint64_t lostCount = 0;
    for (const Value & value : values)
    {
        if (!value.deliveryCount.load(std::memory_order_relaxed))
        {
            ++lostCount;
        }
    }

    uint64_t violationCount = total.wrongQueueCount + total.orderBreakCount + total.duplicateCount + lostCount;
    ASSERT_EQ(violationCount, 0u) << "wrong queue " << total.wrongQueueCount << ", order break " << total.orderBreakCount << ", duplicate "
                                  << total.duplicateCount << ", lost " << lostCount;
}

void QueueStressTest::runProducer(ValueQueue * queue, Value * values) noexcept
{
    for (uint32_t sequence = 0; sequence < VALUES_PER_PRODUCER; ++sequence)
    {
        bool enqueued = queue->enqueue(&values[sequence]);
        SILK_ASSERT(enqueued, "could not allocate a queue node");
    }
}

void QueueStressTest::runConsumer(
    ValueQueue * queue, uint32_t queueIndex, const std::atomic<bool> * producersDone, ConsumerResult * result) noexcept
{
    int64_t lastSequences[PRODUCER_COUNT];
    for (int64_t & lastSequence : lastSequences)
    {
        lastSequence = -1;
    }

    for (;;)
    {
        // An empty dequeue after the flag was read as set proves that the queue is drained.
        bool done = producersDone->load(std::memory_order_acquire);
        Value * value = queue->dequeue();
        if (value)
        {
            checkValue(value, queueIndex, lastSequences, result);
            continue;
        }

        if (done)
        {
            return;
        }

        schedYield();
    }
}

void QueueStressTest::checkValue(Value * value, uint32_t queueIndex, int64_t * lastSequences, ConsumerResult * result) noexcept
{
    uint32_t deliveryCount = value->deliveryCount.fetch_add(1, std::memory_order_relaxed);
    if (deliveryCount)
    {
        ++result->duplicateCount;
    }

    if (value->queueIndex != queueIndex)
    {
        ++result->wrongQueueCount;
    }

    if (value->sequence <= lastSequences[value->producerIndex])
    {
        ++result->orderBreakCount;
    }

    lastSequences[value->producerIndex] = value->sequence;
}

} // namespace silk
