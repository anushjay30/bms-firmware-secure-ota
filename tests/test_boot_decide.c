/*
 * The A/B boot state machine (boot_decide in common/src/boot_meta.c):
 * trial boots, rollback, fallback, anti-rollback, and the app-side rules.
 * Image verification is faked; tests/test_image.c covers the real one.
 */
#include <string.h>

#include "boot_meta.h"
#include "unity.h"

#define V1 0x010000u /* 1.0.0 */
#define V2 0x020000u
#define V3 0x030000u

typedef struct {
    bool     valid[2];
    uint32_t version[2];
    int      calls[2];
} fake_flash_t;

static fake_flash_t f;

static boot_slot_check_t fake_check(uint8_t slot, uint32_t min_version, void *ctx)
{
    fake_flash_t *x = (fake_flash_t *)ctx;
    x->calls[slot]++;
    /* Like bms_image_verify: an image below the floor does not verify. */
    const boot_slot_check_t r = { .valid = x->valid[slot] && x->version[slot] >= min_version,
                                  .version = x->version[slot] };
    return r;
}

static void slot_image(uint8_t slot, bool valid, uint32_t version)
{
    f.valid[slot] = valid;
    f.version[slot] = version;
}

static boot_meta_t confirmed_on(uint8_t slot, uint32_t floor)
{
    boot_meta_t m;
    boot_meta_defaults(&m);
    m.active_slot = slot;
    m.min_version = floor;
    return m;
}

/* One boot: decide, and carry the new state forward like the bootloader
 * persisting it. */
static boot_decision_t boot(boot_meta_t *m)
{
    boot_meta_t next;
    const boot_decision_t d = boot_decide(m, fake_check, &f, &next);
    *m = next;
    return d;
}

void setUp(void)
{
    memset(&f, 0, sizeof f);
}

void tearDown(void) {}

void test_confirmed_image_boots_without_touching_the_metadata(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_NORMAL, d.reason);
    TEST_ASSERT_FALSE(d.meta_changed); /* steady state: no flash wear */
}

void test_first_boot_records_the_factory_image_version_as_the_floor(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    boot_meta_t m;
    boot_meta_defaults(&m); /* blank META */
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, d.slot);
    TEST_ASSERT_TRUE(d.meta_changed);
    TEST_ASSERT_EQUAL_HEX32(V1, m.min_version);
}

void test_staged_image_starts_its_first_trial_boot(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    slot_image(BOOT_SLOT_B, true, V2);
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    boot_meta_stage(&m, BOOT_SLOT_B);

    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_TRIAL, d.reason);
    TEST_ASSERT_EQUAL_UINT8(1, d.attempt);
    TEST_ASSERT_TRUE(d.meta_changed);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, m.active_slot);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_NONE, m.pending_slot);
    TEST_ASSERT_EQUAL_UINT8(0, m.confirmed);
    TEST_ASSERT_EQUAL_UINT8(1, m.boot_attempts);
    TEST_ASSERT_EQUAL_HEX32(V1, m.min_version); /* floor rises only on confirmation */
}

void test_staged_image_that_fails_verification_is_ignored(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    slot_image(BOOT_SLOT_B, false, V2); /* corrupted since the app checked it */
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    boot_meta_stage(&m, BOOT_SLOT_B);

    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_NORMAL, d.reason);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.other_slot);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_NONE, m.pending_slot); /* consumed, not retried forever */
}

void test_staged_older_image_is_refused_by_anti_rollback(void)
{
    slot_image(BOOT_SLOT_A, true, V2);
    slot_image(BOOT_SLOT_B, true, V1); /* validly signed, but older */
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V2);
    boot_meta_stage(&m, BOOT_SLOT_B);
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, d.slot);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.other_slot);
}

void test_trial_image_gets_three_boots_then_rolls_back(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    slot_image(BOOT_SLOT_B, true, V2);
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    boot_meta_stage(&m, BOOT_SLOT_B);

    for (uint8_t n = 1; n <= BOOT_MAX_TRIAL_BOOTS; n++) {
        const boot_decision_t d = boot(&m); /* the app never confirms */
        TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.slot);
        TEST_ASSERT_EQUAL(BOOT_REASON_TRIAL, d.reason);
        TEST_ASSERT_EQUAL_UINT8(n, d.attempt);
    }
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_ROLLBACK, d.reason);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.other_slot);
    TEST_ASSERT_EQUAL_UINT8(1, m.confirmed);
    TEST_ASSERT_EQUAL_UINT8(0, m.boot_attempts);
    TEST_ASSERT_EQUAL_HEX32(V1, m.min_version);

    /* ...and stays there. */
    const boot_decision_t again = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, again.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_NORMAL, again.reason);
    TEST_ASSERT_FALSE(again.meta_changed);
}

void test_confirmed_trial_image_becomes_the_normal_image(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    slot_image(BOOT_SLOT_B, true, V2);
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    boot_meta_stage(&m, BOOT_SLOT_B);
    (void)boot(&m);

    boot_meta_confirm(&m, V2); /* health check passed in the app */
    TEST_ASSERT_EQUAL_HEX32(V2, m.min_version);

    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_NORMAL, d.reason);
    TEST_ASSERT_FALSE(d.meta_changed);
}

void test_trial_image_that_stops_verifying_rolls_back_at_once(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    slot_image(BOOT_SLOT_B, true, V2);
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    boot_meta_stage(&m, BOOT_SLOT_B);
    (void)boot(&m);

    f.valid[BOOT_SLOT_B] = false;
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_ROLLBACK, d.reason);
}

void test_broken_active_image_falls_back_to_a_valid_other_slot(void)
{
    slot_image(BOOT_SLOT_A, false, V2); /* bit rot in the running image */
    slot_image(BOOT_SLOT_B, true, V3);  /* newer image, written but never staged */
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V2);
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_FALLBACK, d.reason);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, d.other_slot);
    TEST_ASSERT_EQUAL_HEX32(V3, m.min_version);
}

void test_an_older_image_is_never_a_fallback(void)
{
    slot_image(BOOT_SLOT_A, false, V2);
    slot_image(BOOT_SLOT_B, true, V1); /* the previous release: below the floor */
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V2);
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_NONE, d.slot);
    TEST_ASSERT_EQUAL(BOOT_REASON_NONE, d.reason);
}

void test_no_valid_image_means_no_boot(void)
{
    boot_meta_t m;
    boot_meta_defaults(&m);
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_NONE, d.slot);
}

void test_rollback_to_a_broken_old_image_keeps_the_new_one(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    slot_image(BOOT_SLOT_B, true, V2);
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    boot_meta_stage(&m, BOOT_SLOT_B);
    for (unsigned i = 0; i < BOOT_MAX_TRIAL_BOOTS; i++) {
        (void)boot(&m);
    }
    f.valid[BOOT_SLOT_A] = false; /* the rollback target is gone too */
    const boot_decision_t d = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, d.slot); /* better than nothing */
    TEST_ASSERT_EQUAL(BOOT_REASON_FALLBACK, d.reason);
}

void test_each_slot_is_verified_at_most_once_per_boot(void)
{
    slot_image(BOOT_SLOT_A, true, V1);
    slot_image(BOOT_SLOT_B, true, V2);
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    boot_meta_stage(&m, BOOT_SLOT_B);
    (void)boot(&m); /* verifies B for staging, then boots it */
    TEST_ASSERT_EQUAL_INT(1, f.calls[BOOT_SLOT_B]);
    TEST_ASSERT_EQUAL_INT(0, f.calls[BOOT_SLOT_A]);
}

void test_update_is_only_allowed_from_a_confirmed_consistent_state(void)
{
    boot_meta_t m = confirmed_on(BOOT_SLOT_A, V1);
    TEST_ASSERT_TRUE(boot_meta_update_allowed(&m, BOOT_SLOT_A));
    TEST_ASSERT_FALSE(boot_meta_update_allowed(&m, BOOT_SLOT_B)); /* metadata disagrees */

    m.confirmed = 0u; /* on trial: the other slot is the rollback target */
    TEST_ASSERT_FALSE(boot_meta_update_allowed(&m, BOOT_SLOT_A));

    m.confirmed = 1u;
    boot_meta_stage(&m, BOOT_SLOT_B); /* already staged, waiting for a reboot */
    TEST_ASSERT_FALSE(boot_meta_update_allowed(&m, BOOT_SLOT_A));
}

void test_full_lifecycle(void)
{
    /* Factory: v1 in A. */
    slot_image(BOOT_SLOT_A, true, V1);
    boot_meta_t m;
    boot_meta_defaults(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, boot(&m).slot);

    /* Update 1: v2 into B, trial, confirmed. */
    slot_image(BOOT_SLOT_B, true, V2);
    TEST_ASSERT_TRUE(boot_meta_update_allowed(&m, BOOT_SLOT_A));
    boot_meta_stage(&m, BOOT_SLOT_B);
    TEST_ASSERT_EQUAL(BOOT_REASON_TRIAL, boot(&m).reason);
    boot_meta_confirm(&m, V2);

    /* Update 2: v3 into A, never confirms -> back to v2 in B. */
    slot_image(BOOT_SLOT_A, true, V3);
    TEST_ASSERT_TRUE(boot_meta_update_allowed(&m, BOOT_SLOT_B));
    boot_meta_stage(&m, BOOT_SLOT_A);
    for (unsigned i = 0; i < BOOT_MAX_TRIAL_BOOTS; i++) {
        TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, boot(&m).slot);
    }
    const boot_decision_t rb = boot(&m);
    TEST_ASSERT_EQUAL(BOOT_REASON_ROLLBACK, rb.reason);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, rb.slot);

    /* Downgrade attempt: v1 into A, staged - refused, v2 keeps running. */
    slot_image(BOOT_SLOT_A, true, V1);
    boot_meta_stage(&m, BOOT_SLOT_A);
    const boot_decision_t dg = boot(&m);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, dg.slot);
    TEST_ASSERT_EQUAL_HEX32(V2, m.min_version);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_confirmed_image_boots_without_touching_the_metadata);
    RUN_TEST(test_first_boot_records_the_factory_image_version_as_the_floor);
    RUN_TEST(test_staged_image_starts_its_first_trial_boot);
    RUN_TEST(test_staged_image_that_fails_verification_is_ignored);
    RUN_TEST(test_staged_older_image_is_refused_by_anti_rollback);
    RUN_TEST(test_trial_image_gets_three_boots_then_rolls_back);
    RUN_TEST(test_confirmed_trial_image_becomes_the_normal_image);
    RUN_TEST(test_trial_image_that_stops_verifying_rolls_back_at_once);
    RUN_TEST(test_broken_active_image_falls_back_to_a_valid_other_slot);
    RUN_TEST(test_an_older_image_is_never_a_fallback);
    RUN_TEST(test_no_valid_image_means_no_boot);
    RUN_TEST(test_rollback_to_a_broken_old_image_keeps_the_new_one);
    RUN_TEST(test_each_slot_is_verified_at_most_once_per_boot);
    RUN_TEST(test_update_is_only_allowed_from_a_confirmed_consistent_state);
    RUN_TEST(test_full_lifecycle);
    return UNITY_END();
}
