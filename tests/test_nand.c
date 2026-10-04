/* =============================================================================
 * test_nand.c -- Does the simulated die actually behave like NAND?
 * =============================================================================
 *
 * Every other test in this project rests on this file. If the device layer is
 * permissive where real hardware is strict, the FTL above it can pass its whole
 * test suite and still be unshippable, because it is relying on behaviour the
 * silicon does not offer. So these tests are mostly about proving the die says
 * NO in all the places it should.
 * ========================================================================== */

#include "test_framework.h"
#include "nand.h"

static uint8_t buf[NAND_PAGE_SIZE_BYTES];
static uint8_t out[NAND_PAGE_SIZE_BYTES];

static void spare_init(nand_spare_t *s, uint32_t lpn, uint32_t seq)
{
    s->lpn = lpn;
    s->seq = seq;
    s->crc = 0u;
    s->tag = NAND_TAG_USER_DATA;
}

/* A fresh die must be entirely erased, with zero erase counts. */
TEST_CASE(test_format_gives_a_blank_die)
{
    uint32_t pbn, good = 0u;

    nand_format(42u);

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        CHECK(nand_block_write_pointer(pbn) == 0u);
        CHECK(nand_block_erase_count(pbn) == 0u);
        if (!nand_block_is_bad(pbn)) {
            good++;
        }
    }
    CHECK_EQ_U(NAND_NUM_BLOCKS - good, NAND_FACTORY_BAD_BLOCKS);

    /* Factory bad blocks must never land in the metadata region: firmware has
     * to be able to find its own metadata at a fixed address before it knows
     * anything else about the die. */
    for (pbn = 0u; pbn < FTL_META_BLOCKS; pbn++) {
        CHECK(!nand_block_is_factory_bad(pbn));
    }
}

/* Reading a page that was never programmed is an error, not zeros. */
TEST_CASE(test_read_of_erased_page_fails)
{
    nand_format(1u);
    CHECK(nand_read_page(BLOCK_OFFSET_TO_PPN(FTL_DATA_BLOCK_FIRST, 0u), out, NULL)
          == NAND_ERR_READ_UNWRITTEN);
}

/* RULE 1: program-once. This is the defining property of NAND. */
TEST_CASE(test_program_once_is_enforced)
{
    nand_spare_t sp;
    uint32_t     ppn = BLOCK_OFFSET_TO_PPN(FTL_DATA_BLOCK_FIRST, 0u);

    nand_format(1u);
    test_fill_pattern(buf, 7u, 1u);
    spare_init(&sp, 7u, 1u);

    CHECK(nand_program_page(ppn, buf, &sp) == NAND_OK);
    /* Second program of the same page: refused. No overwrite, ever. */
    CHECK(nand_program_page(ppn, buf, &sp) == NAND_ERR_NOT_ERASED);
}

/* RULE 2: pages within a block must be programmed in ascending order. */
TEST_CASE(test_sequential_programming_is_enforced)
{
    nand_spare_t sp;
    uint32_t     b = FTL_DATA_BLOCK_FIRST;

    nand_format(1u);
    test_fill_pattern(buf, 0u, 1u);
    spare_init(&sp, 0u, 1u);

    /* Skipping ahead to offset 3 while 0..2 are still erased is illegal. */
    CHECK(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 3u), buf, &sp)
          == NAND_ERR_OUT_OF_ORDER);

    CHECK(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 0u), buf, &sp) == NAND_OK);
    CHECK(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 1u), buf, &sp) == NAND_OK);
    CHECK_EQ_U(nand_block_write_pointer(b), 2u);
}

/* Data written must come back byte-for-byte, spare area included. */
TEST_CASE(test_program_then_read_round_trip)
{
    nand_spare_t sp, got;
    uint32_t     ppn = BLOCK_OFFSET_TO_PPN(FTL_DATA_BLOCK_FIRST, 0u);

    nand_format(1u);
    test_fill_pattern(buf, 1234u, 9u);
    spare_init(&sp, 1234u, 555u);

    REQUIRE(nand_program_page(ppn, buf, &sp) == NAND_OK);
    REQUIRE(nand_read_page(ppn, out, &got) == NAND_OK);

    CHECK(test_check_pattern(out, 1234u, 9u));
    CHECK_EQ_U(got.lpn, 1234u);
    CHECK_EQ_U(got.seq, 555u);
    CHECK_EQ_U(got.tag, NAND_TAG_USER_DATA);
    CHECK(got.crc == nand_crc32(buf, NAND_PAGE_SIZE_BYTES));

    /* A spare-only read must agree with the full read but cost less. */
    {
        const nand_counters_t *nc = nand_counters();
        uint64_t before = nc->busy_us;
        nand_spare_t sp2;
        CHECK(nand_read_spare(ppn, &sp2) == NAND_OK);
        CHECK_EQ_U(sp2.lpn, 1234u);
        CHECK((nc->busy_us - before) < NAND_TREAD_US);
    }
}

/* RULE 3: erase works on whole blocks and resets the write pointer. */
TEST_CASE(test_erase_clears_the_block_and_counts)
{
    nand_spare_t sp;
    uint32_t     b = FTL_DATA_BLOCK_FIRST, i;

    nand_format(1u);
    test_fill_pattern(buf, 0u, 1u);
    spare_init(&sp, 0u, 1u);

    for (i = 0u; i < 5u; i++) {
        CHECK(nand_program_page(BLOCK_OFFSET_TO_PPN(b, i), buf, &sp) == NAND_OK);
    }
    CHECK_EQ_U(nand_block_write_pointer(b), 5u);

    CHECK(nand_erase_block(b) == NAND_OK);
    CHECK_EQ_U(nand_block_write_pointer(b), 0u);
    CHECK_EQ_U(nand_block_erase_count(b), 1u);
    CHECK(nand_read_page(BLOCK_OFFSET_TO_PPN(b, 0u), out, NULL)
          == NAND_ERR_READ_UNWRITTEN);

    /* And the block is programmable again. */
    CHECK(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 0u), buf, &sp) == NAND_OK);
}

/* Bad blocks reject everything. */
TEST_CASE(test_bad_blocks_reject_all_operations)
{
    nand_spare_t sp;
    uint32_t     b = FTL_DATA_BLOCK_FIRST + 1u;

    nand_format(1u);
    spare_init(&sp, 0u, 1u);
    test_fill_pattern(buf, 0u, 1u);

    CHECK(!nand_block_is_bad(b));
    nand_mark_bad(b);
    CHECK(nand_block_is_bad(b));
    CHECK(nand_block_is_runtime_bad(b));

    CHECK(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 0u), buf, &sp) == NAND_ERR_BAD_BLOCK);
    CHECK(nand_read_page(BLOCK_OFFSET_TO_PPN(b, 0u), out, NULL) == NAND_ERR_BAD_BLOCK);
    CHECK(nand_erase_block(b) == NAND_ERR_BAD_BLOCK);
}

/* Endurance: past the rating the block refuses to erase. */
TEST_CASE(test_endurance_limit_is_enforced)
{
    uint32_t b = FTL_DATA_BLOCK_FIRST, i;

    nand_format(1u);
    for (i = 0u; i < NAND_MAX_ERASE_CYCLES; i++) {
        REQUIRE(nand_erase_block(b) == NAND_OK);
    }
    CHECK_EQ_U(nand_block_erase_count(b), NAND_MAX_ERASE_CYCLES);
    CHECK(nand_erase_block(b) == NAND_ERR_WORN_OUT);
}

/* A failed program consumes the page but leaves the block usable and readable,
 * so firmware can rescue the earlier pages before retiring it. */
TEST_CASE(test_injected_program_failure_consumes_the_page)
{
    nand_spare_t sp;
    uint32_t     b = FTL_DATA_BLOCK_FIRST;
    nand_err_t   err = NAND_OK;
    uint32_t     i;

    nand_format(7u);
    spare_init(&sp, 0u, 1u);
    test_fill_pattern(buf, 0u, 1u);

    /* Write one good page first: it is the thing that must survive. */
    REQUIRE(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 0u), buf, &sp) == NAND_OK);

    nand_set_failure_rates(1000000u, 0u);   /* every program fails */
    for (i = 1u; i < 4u; i++) {
        err = nand_program_page(BLOCK_OFFSET_TO_PPN(b, i), buf, &sp);
        CHECK(err == NAND_ERR_PROGRAM_FAIL);
    }
    nand_set_failure_rates(0u, 0u);

    /* The write pointer advanced (the slots are gone) but the block is NOT
     * automatically bad, and page 0 still reads back correctly. */
    CHECK_EQ_U(nand_block_write_pointer(b), 4u);
    CHECK(!nand_block_is_bad(b));
    CHECK(nand_read_page(BLOCK_OFFSET_TO_PPN(b, 0u), out, NULL) == NAND_OK);
    CHECK(test_check_pattern(out, 0u, 1u));

    /* A page whose program failed reads back as an uncorrectable ECC error. */
    CHECK(nand_read_page(BLOCK_OFFSET_TO_PPN(b, 1u), out, NULL)
          == NAND_ERR_ECC_UNCORRECTABLE);
}

/* Read-disturb counting, and the fact that an erase resets it. */
TEST_CASE(test_read_disturb_counter)
{
    nand_spare_t sp;
    uint32_t     b = FTL_DATA_BLOCK_FIRST, i;

    nand_format(1u);
    spare_init(&sp, 0u, 1u);
    test_fill_pattern(buf, 0u, 1u);
    REQUIRE(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 0u), buf, &sp) == NAND_OK);

    for (i = 0u; i < NAND_READ_DISTURB_LIMIT; i++) {
        (void)nand_read_spare(BLOCK_OFFSET_TO_PPN(b, 0u), &sp);
    }
    CHECK(nand_block_read_disturb_exceeded(b));

    CHECK(nand_erase_block(b) == NAND_OK);
    CHECK(!nand_block_read_disturb_exceeded(b));
    CHECK_EQ_U(nand_block_read_count(b), 0u);
}

/* Non-volatility: data and durable metadata survive a power cycle; the volatile
 * read-disturb counters, which live in controller SRAM on a real product, do not. */
TEST_CASE(test_power_cycle_preserves_media)
{
    nand_spare_t sp;
    uint32_t     b = FTL_DATA_BLOCK_FIRST;

    nand_format(3u);
    spare_init(&sp, 99u, 12345u);
    test_fill_pattern(buf, 99u, 4u);
    REQUIRE(nand_program_page(BLOCK_OFFSET_TO_PPN(b, 0u), buf, &sp) == NAND_OK);
    (void)nand_read_spare(BLOCK_OFFSET_TO_PPN(b, 0u), &sp);
    CHECK(nand_block_read_count(b) > 0u);

    nand_power_cycle();

    CHECK_EQ_U(nand_block_read_count(b), 0u);            /* volatile: gone     */
    CHECK_EQ_U(nand_block_write_pointer(b), 1u);         /* durable: preserved */
    REQUIRE(nand_read_page(BLOCK_OFFSET_TO_PPN(b, 0u), out, &sp) == NAND_OK);
    CHECK(test_check_pattern(out, 99u, 4u));
    CHECK_EQ_U(sp.seq, 12345u);
}

/* CRC-32 must match the known value for a standard test vector, otherwise the
 * "ECC" protecting every page is protecting nothing. */
TEST_CASE(test_crc32_matches_known_vector)
{
    CHECK(nand_crc32("123456789", 9u) == 0xCBF43926u);
    CHECK(nand_crc32("", 0u) == 0u);
}

void suite_nand(void)
{
    SUITE_BEGIN("nand -- the simulated die");
    RUN_TEST(test_format_gives_a_blank_die);
    RUN_TEST(test_read_of_erased_page_fails);
    RUN_TEST(test_program_once_is_enforced);
    RUN_TEST(test_sequential_programming_is_enforced);
    RUN_TEST(test_program_then_read_round_trip);
    RUN_TEST(test_erase_clears_the_block_and_counts);
    RUN_TEST(test_bad_blocks_reject_all_operations);
    RUN_TEST(test_endurance_limit_is_enforced);
    RUN_TEST(test_injected_program_failure_consumes_the_page);
    RUN_TEST(test_read_disturb_counter);
    RUN_TEST(test_power_cycle_preserves_media);
    RUN_TEST(test_crc32_matches_known_vector);
}
