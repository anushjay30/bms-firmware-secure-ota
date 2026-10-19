#include <pthread.h>

#include "can_fifo.h"
#include "unity.h"

static can_fifo_t fifo;

void setUp(void)
{
    can_fifo_init(&fifo);
}

void tearDown(void) {}

static can_frame_t frame(uint32_t seq)
{
    can_frame_t f = { .id = seq & 0x7FFu, .dlc = 4u, .data = { 0 } };
    f.data[0] = (uint8_t)seq;
    f.data[1] = (uint8_t)(seq >> 8);
    f.data[2] = (uint8_t)(seq >> 16);
    f.data[3] = (uint8_t)(seq >> 24);
    return f;
}

static uint32_t seq_of(const can_frame_t *f)
{
    return (uint32_t)f->data[0] | ((uint32_t)f->data[1] << 8) | ((uint32_t)f->data[2] << 16) |
           ((uint32_t)f->data[3] << 24);
}

void test_empty_fifo_has_nothing_to_peek(void)
{
    can_frame_t out;
    TEST_ASSERT_FALSE(can_fifo_peek(&fifo, &out));
    TEST_ASSERT_EQUAL_UINT(0, can_fifo_count(&fifo));
}

void test_frames_come_out_in_order(void)
{
    for (uint32_t i = 0; i < 10u; i++) {
        const can_frame_t in = frame(i);
        TEST_ASSERT_TRUE(can_fifo_push(&fifo, &in));
    }
    for (uint32_t i = 0; i < 10u; i++) {
        can_frame_t out;
        TEST_ASSERT_TRUE(can_fifo_peek(&fifo, &out));
        TEST_ASSERT_EQUAL_UINT32(i, seq_of(&out));
        can_fifo_pop(&fifo);
    }
    TEST_ASSERT_EQUAL_UINT(0, can_fifo_count(&fifo));
}

void test_full_fifo_rejects_new_frames(void)
{
    const can_frame_t in = frame(1);
    for (unsigned i = 0; i < CAN_FIFO_CAPACITY; i++) {
        TEST_ASSERT_TRUE(can_fifo_push(&fifo, &in));
    }
    TEST_ASSERT_FALSE(can_fifo_push(&fifo, &in)); /* overrun */
    TEST_ASSERT_EQUAL_UINT(CAN_FIFO_CAPACITY, can_fifo_count(&fifo));
}

void test_indices_wrap_many_times(void)
{
    for (uint32_t i = 0; i < 10u * CAN_FIFO_CAPACITY + 3u; i++) {
        const can_frame_t in = frame(i);
        can_frame_t out;
        TEST_ASSERT_TRUE(can_fifo_push(&fifo, &in));
        TEST_ASSERT_TRUE(can_fifo_peek(&fifo, &out));
        TEST_ASSERT_EQUAL_UINT32(i, seq_of(&out));
        can_fifo_pop(&fifo);
    }
}

/* Concurrency: one producer thread, one consumer thread, 200k frames. Every
 * frame must arrive exactly once and in order. Run under ThreadSanitizer or
 * AddressSanitizer this also catches data races / torn reads. */
#define STRESS_FRAMES 200000u

static void *producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < STRESS_FRAMES;) {
        const can_frame_t in = frame(i);
        if (can_fifo_push(&fifo, &in)) {
            i++;
        }
    }
    return NULL;
}

void test_concurrent_producer_and_consumer(void)
{
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, producer, NULL));

    uint32_t expected = 0u;
    while (expected < STRESS_FRAMES) {
        can_frame_t out;
        if (can_fifo_peek(&fifo, &out)) {
            TEST_ASSERT_EQUAL_UINT32(expected, seq_of(&out));
            can_fifo_pop(&fifo);
            expected++;
        }
    }
    TEST_ASSERT_EQUAL_INT(0, pthread_join(t, NULL));
    TEST_ASSERT_EQUAL_UINT(0, can_fifo_count(&fifo));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_fifo_has_nothing_to_peek);
    RUN_TEST(test_frames_come_out_in_order);
    RUN_TEST(test_full_fifo_rejects_new_frames);
    RUN_TEST(test_indices_wrap_many_times);
    RUN_TEST(test_concurrent_producer_and_consumer);
    return UNITY_END();
}
