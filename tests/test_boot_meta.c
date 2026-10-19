/*
 * Two-copy boot metadata on the emulated flash (common/src/boot_meta.c):
 * newest-valid-copy selection, corrupted copies, and power cuts at every
 * byte of a write.
 */
#include <string.h>

#include "boot_meta.h"
#include "flash_hal.h"
#include "unity.h"

#define META_BASE 0x00010000u
#define META_SIZE (4u * FLASH_PAGE_SIZE)

static uint8_t mem[META_SIZE];
static const boot_meta_store_t store = { { META_BASE, META_BASE + FLASH_PAGE_SIZE } };

static void reset_flash(uint8_t fill)
{
    memset(mem, fill, sizeof mem);
    flash_emul_init((uintptr_t)mem, META_BASE, META_SIZE);
}

static boot_meta_t meta_with(uint8_t active, uint32_t min_version)
{
    boot_meta_t m;
    boot_meta_defaults(&m);
    m.active_slot = active;
    m.min_version = min_version;
    return m;
}

static boot_meta_view_t load(void)
{
    boot_meta_view_t v;
    boot_meta_load(&store, &v);
    return v;
}

/* Store a state, starting from whatever is in flash now. */
static void save(boot_meta_t m)
{
    boot_meta_view_t v = load();
    TEST_ASSERT_EQUAL(FLASH_OK, boot_meta_save(&store, &v, m));
}

void setUp(void)
{
    reset_flash(0xFF);
}

void tearDown(void) {}

void test_blank_flash_gives_factory_defaults(void)
{
    const boot_meta_view_t v = load();
    TEST_ASSERT_EQUAL_INT(-1, v.copy);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, v.meta.active_slot);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_NONE, v.meta.pending_slot);
    TEST_ASSERT_EQUAL_UINT8(1, v.meta.confirmed);
    TEST_ASSERT_EQUAL_UINT32(0, v.meta.min_version);
}

void test_zeroed_flash_is_not_mistaken_for_a_record(void)
{
    reset_flash(0x00);
    TEST_ASSERT_EQUAL_INT(-1, load().copy);
}

void test_first_write_goes_to_copy_0_then_alternates(void)
{
    save(meta_with(BOOT_SLOT_B, 1));
    boot_meta_view_t v = load();
    TEST_ASSERT_EQUAL_INT(0, v.copy);
    TEST_ASSERT_EQUAL_UINT32(1, v.meta.sequence);

    save(meta_with(BOOT_SLOT_A, 2));
    v = load();
    TEST_ASSERT_EQUAL_INT(1, v.copy);
    TEST_ASSERT_EQUAL_UINT32(2, v.meta.sequence);
    TEST_ASSERT_TRUE(v.valid[0] && v.valid[1]); /* the previous state is still there */

    save(meta_with(BOOT_SLOT_B, 3));
    v = load();
    TEST_ASSERT_EQUAL_INT(0, v.copy);
    TEST_ASSERT_EQUAL_UINT32(3, v.meta.sequence);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, v.meta.active_slot);
    TEST_ASSERT_EQUAL_UINT32(3, v.meta.min_version);
}

void test_save_updates_the_view_in_place(void)
{
    boot_meta_view_t v = load();
    TEST_ASSERT_EQUAL(FLASH_OK, boot_meta_save(&store, &v, meta_with(BOOT_SLOT_B, 7)));
    TEST_ASSERT_EQUAL(FLASH_OK, boot_meta_save(&store, &v, meta_with(BOOT_SLOT_A, 8)));
    const boot_meta_view_t again = load();
    TEST_ASSERT_EQUAL_INT(again.copy, v.copy);
    TEST_ASSERT_EQUAL_MEMORY(&again.meta, &v.meta, sizeof v.meta);
}

void test_corrupted_newest_copy_falls_back_to_the_older(void)
{
    save(meta_with(BOOT_SLOT_A, 10));
    save(meta_with(BOOT_SLOT_B, 20)); /* newest, in copy 1 */
    mem[FLASH_PAGE_SIZE + 9u] ^= 0x04u; /* one bit of it rots */
    const boot_meta_view_t v = load();
    TEST_ASSERT_FALSE(v.valid[1]);
    TEST_ASSERT_EQUAL_INT(0, v.copy);
    TEST_ASSERT_EQUAL_UINT32(10, v.meta.min_version);
}

void test_both_copies_corrupted_gives_defaults(void)
{
    save(meta_with(BOOT_SLOT_B, 10));
    save(meta_with(BOOT_SLOT_B, 20));
    mem[3] ^= 0xFFu;
    mem[FLASH_PAGE_SIZE + 3u] ^= 0xFFu;
    const boot_meta_view_t v = load();
    TEST_ASSERT_EQUAL_INT(-1, v.copy);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_A, v.meta.active_slot);
}

void test_record_with_valid_crc_but_impossible_slot_is_rejected(void)
{
    boot_meta_t m = meta_with(BOOT_SLOT_A, 1);
    m.active_slot = 7; /* the CRC will be computed over this nonsense */
    boot_meta_view_t v = load();
    TEST_ASSERT_EQUAL(FLASH_OK, boot_meta_save(&store, &v, m));
    TEST_ASSERT_EQUAL_INT(-1, load().copy);
}

void test_sequence_comparison_survives_wraparound(void)
{
    boot_meta_view_t v = load();
    v.copy = 1;                    /* pretend copy 1 holds sequence 0xFFFFFFFE, */
    v.meta.sequence = 0xFFFFFFFEu; /* so the next write (to copy 0) gets 0xFFFFFFFF */
    TEST_ASSERT_EQUAL(FLASH_OK, boot_meta_save(&store, &v, meta_with(BOOT_SLOT_A, 1)));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, load().meta.sequence);

    TEST_ASSERT_EQUAL(FLASH_OK, boot_meta_save(&store, &v, meta_with(BOOT_SLOT_B, 2))); /* wraps to 0 */
    const boot_meta_view_t after = load();
    TEST_ASSERT_EQUAL_INT(1, after.copy); /* 0 is newer than 0xFFFFFFFF */
    TEST_ASSERT_EQUAL_UINT32(0u, after.meta.sequence);
    TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, after.meta.active_slot);
}

/*
 * The property the whole scheme exists for: interrupt a metadata write after
 * every possible number of bytes - during the erase of the target page and
 * during the programming of the record - and check that what the next boot
 * reads is always exactly the old state or exactly the new one.
 */
void test_power_cut_at_every_byte_of_a_write_never_loses_the_state(void)
{
    const boot_meta_t old_state = meta_with(BOOT_SLOT_A, 0x010000u);
    const boot_meta_t new_state = meta_with(BOOT_SLOT_B, 0x020000u);

    for (uint32_t cut = 0; cut <= sizeof(boot_meta_t); cut++) {
        reset_flash(0xFF);
        save(meta_with(BOOT_SLOT_A, 1)); /* older history in copy 0 */
        save(old_state);                 /* current state, copy 1   */

        boot_meta_view_t v = load();
        flash_emul_inject(FLASH_FAULT_POWER_LOSS, cut);
        const flash_status_t st = boot_meta_save(&store, &v, new_state); /* targets copy 0 */
        flash_emul_power_on();

        const boot_meta_view_t after = load();
        TEST_ASSERT_TRUE_MESSAGE(after.copy >= 0, "both copies lost");
        if (cut < sizeof(boot_meta_t)) {
            TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, st);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(BOOT_SLOT_A, after.meta.active_slot, "torn write was taken as valid");
            TEST_ASSERT_EQUAL_UINT32(0x010000u, after.meta.min_version);
        } else {
            TEST_ASSERT_EQUAL(FLASH_OK, st); /* the whole record made it */
            TEST_ASSERT_EQUAL_UINT8(BOOT_SLOT_B, after.meta.active_slot);
            TEST_ASSERT_EQUAL_UINT32(0x020000u, after.meta.min_version);
        }
    }
}

void test_power_cut_during_the_erase_never_loses_the_state(void)
{
    static const uint32_t cuts[] = { 0u, 1u, 16u, 31u, 32u, 100u, FLASH_PAGE_SIZE / 2u, FLASH_PAGE_SIZE - 1u };
    for (unsigned i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        reset_flash(0xFF);
        save(meta_with(BOOT_SLOT_B, 5)); /* copy 0 */
        save(meta_with(BOOT_SLOT_A, 6)); /* copy 1: current */

        boot_meta_view_t v = load();
        flash_emul_inject(FLASH_FAULT_POWER_LOSS_ERASE, cuts[i]);
        TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, boot_meta_save(&store, &v, meta_with(BOOT_SLOT_B, 7)));
        flash_emul_power_on();

        const boot_meta_view_t after = load();
        TEST_ASSERT_EQUAL_INT(1, after.copy);
        TEST_ASSERT_EQUAL_UINT32(6, after.meta.min_version);
    }
}

void test_after_a_torn_write_the_next_write_still_works(void)
{
    save(meta_with(BOOT_SLOT_A, 1));
    boot_meta_view_t v = load();
    flash_emul_inject(FLASH_FAULT_POWER_LOSS, 12u);
    TEST_ASSERT_EQUAL(FLASH_ERR_POWER_LOSS, boot_meta_save(&store, &v, meta_with(BOOT_SLOT_B, 2)));
    flash_emul_power_on();

    save(meta_with(BOOT_SLOT_B, 3)); /* rewrites the torn copy */
    const boot_meta_view_t after = load();
    TEST_ASSERT_TRUE(after.valid[0] && after.valid[1]);
    TEST_ASSERT_EQUAL_UINT32(3, after.meta.min_version);
}

void test_injected_program_failure_is_reported_and_keeps_the_old_state(void)
{
    save(meta_with(BOOT_SLOT_A, 1));
    boot_meta_view_t v = load();
    flash_emul_inject(FLASH_FAULT_PROGRAM_FAIL, 8u);
    TEST_ASSERT_EQUAL(FLASH_ERR_FAULT, boot_meta_save(&store, &v, meta_with(BOOT_SLOT_B, 2)));
    TEST_ASSERT_EQUAL_UINT32(1, v.meta.min_version); /* view untouched */
    TEST_ASSERT_EQUAL_UINT32(1, load().meta.min_version);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blank_flash_gives_factory_defaults);
    RUN_TEST(test_zeroed_flash_is_not_mistaken_for_a_record);
    RUN_TEST(test_first_write_goes_to_copy_0_then_alternates);
    RUN_TEST(test_save_updates_the_view_in_place);
    RUN_TEST(test_corrupted_newest_copy_falls_back_to_the_older);
    RUN_TEST(test_both_copies_corrupted_gives_defaults);
    RUN_TEST(test_record_with_valid_crc_but_impossible_slot_is_rejected);
    RUN_TEST(test_sequence_comparison_survives_wraparound);
    RUN_TEST(test_power_cut_at_every_byte_of_a_write_never_loses_the_state);
    RUN_TEST(test_power_cut_during_the_erase_never_loses_the_state);
    RUN_TEST(test_after_a_torn_write_the_next_write_still_works);
    RUN_TEST(test_injected_program_failure_is_reported_and_keeps_the_old_state);
    return UNITY_END();
}
