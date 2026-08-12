// SPDX-License-Identifier: GPL-2.0-or-later

#include <kunit/test.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/string.h>

#include "crystal_sddc_codec.h"
#include "lz4kd/lz4kd.h"

struct crystal_sddc_codec_test_ctx {
	struct crystal_sddc_codec *codec;
};

static int crystal_sddc_codec_test_init(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->codec = crystal_sddc_codec_create();
	if (IS_ERR(ctx->codec))
		return PTR_ERR(ctx->codec);

	test->priv = ctx;
	return 0;
}

static void crystal_sddc_codec_test_exit(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;

	if (!ctx)
		return;

	crystal_sddc_codec_destroy(ctx->codec);
}

static void crystal_sddc_fill_prandom(u8 *buf, size_t len, u32 seed)
{
	u32 state = seed;
	size_t i;

	for (i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		buf[i] = state;
	}
}

static u8 *crystal_sddc_test_emit_size(u8 *out, unsigned int size)
{
	while (size >= 255) {
		*out++ = 255;
		size -= 255;
	}
	*out++ = size;
	return out;
}

static size_t crystal_sddc_test_emit_tuple(u8 *out, unsigned int offset,
		const u8 *literals, unsigned int literal_len,
		unsigned int repeat_len)
{
	const unsigned int nr_mask = (1U << 6) - 1;
	const unsigned int r_mask = (1U << 6) - 1;
	unsigned int tag = offset;
	u8 *at = out;

	if (literal_len < nr_mask)
		tag |= literal_len << 18;
	else
		tag |= nr_mask << 18;
	if (repeat_len - 4 < r_mask)
		tag |= (repeat_len - 4) << 12;
	else
		tag |= r_mask << 12;

	*at++ = tag;
	*at++ = tag >> 8;
	*at++ = tag >> 16;
	if (literal_len >= nr_mask)
		at = crystal_sddc_test_emit_size(at, literal_len - nr_mask);
	memcpy(at, literals, literal_len);
	at += literal_len;
	if (repeat_len - 4 >= r_mask)
		at = crystal_sddc_test_emit_size(at, repeat_len - 4 - r_mask);
	return at - out;
}

static void crystal_sddc_lz4kd_small_offset_test(struct kunit *test)
{
	u8 encoded[64];
	u8 literals[15];
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	unsigned int offset;

	KUNIT_ASSERT_NOT_NULL(test, decoded);
	for (offset = 1; offset <= ARRAY_SIZE(literals); offset++) {
		size_t encoded_len;
		unsigned int literal_len = offset == 4 ? offset + 1 : offset;
		unsigned int i;
		int ret;

		for (i = 0; i < literal_len; i++)
			literals[i] = 0x30 + (i % offset);
		memset(encoded, 0, sizeof(encoded));
		memset(decoded, 0xa5, PAGE_SIZE);
		encoded_len = 1 + crystal_sddc_test_emit_tuple(encoded + 1,
				offset, literals, literal_len, 64);

		ret = crystal_lz4kd_decode(encoded, decoded, encoded_len, PAGE_SIZE);
		KUNIT_ASSERT_EQ_MSG(test, ret, (int)(literal_len + 64),
				"offset=%u", offset);
		for (i = 0; i < literal_len + 64; i++)
			KUNIT_EXPECT_EQ_MSG(test, decoded[i], literals[i % offset],
					"offset=%u byte=%u", offset, i);
	}
}

static void crystal_sddc_lz4kd_page_tail_match_test(struct kunit *test)
{
	const u8 first_literal = 0x5a;
	const u8 tail_literals[] = { 0xa1, 0xb2, 0xc3, 0xd4 };
	u8 encoded[64] = { 0 };
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	size_t encoded_len = 1;
	unsigned int i;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, decoded);
	encoded_len += crystal_sddc_test_emit_tuple(encoded + encoded_len, 1,
			&first_literal, 1, PAGE_SIZE - 17);
	encoded_len += crystal_sddc_test_emit_tuple(encoded + encoded_len, 15,
			tail_literals, ARRAY_SIZE(tail_literals), 12);

	memset(decoded, 0xa5, PAGE_SIZE);
	ret = crystal_lz4kd_decode(encoded, decoded, encoded_len, PAGE_SIZE);
	KUNIT_ASSERT_EQ(test, ret, PAGE_SIZE);
	for (i = 0; i < PAGE_SIZE - 16; i++)
		KUNIT_EXPECT_EQ(test, decoded[i], first_literal);
	KUNIT_EXPECT_MEMEQ(test, decoded + PAGE_SIZE - 16, tail_literals,
			ARRAY_SIZE(tail_literals));
	for (i = PAGE_SIZE - 12; i < PAGE_SIZE - 1; i++)
		KUNIT_EXPECT_EQ(test, decoded[i], first_literal);
	KUNIT_EXPECT_EQ(test, decoded[PAGE_SIZE - 1], tail_literals[0]);
}

static void crystal_sddc_lz4kd_delta_reference_start_test(struct kunit *test)
{
	enum { REF_LEN = 64, TARGET_LEN = 64 };
	u8 *state = kunit_kmalloc(test,
			crystal_lz4kd_encode_state_bytes_min(), GFP_KERNEL);
	u8 *window = kunit_kmalloc(test, 1 + REF_LEN + TARGET_LEN, GFP_KERNEL);
	u8 *decode_window = kunit_kmalloc(test, REF_LEN + TARGET_LEN, GFP_KERNEL);
	u8 *encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *ref;
	u8 *target;
	int encoded_len;
	int decoded_len;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, window);
	KUNIT_ASSERT_NOT_NULL(test, decode_window);
	KUNIT_ASSERT_NOT_NULL(test, encoded);
	ref = window + 1;
	target = ref + REF_LEN;
	crystal_sddc_fill_prandom(ref, REF_LEN, 0x41c6ce57);
	window[0] = 0xee;
	target[0] = window[0];
	memcpy(target + 1, ref, TARGET_LEN - 1);

	encoded_len = crystal_lz4kd_encode_delta(state, ref, target, encoded,
			TARGET_LEN, 2 * PAGE_SIZE, PAGE_SIZE);
	KUNIT_ASSERT_GT(test, encoded_len, 0);
	memcpy(decode_window, ref, REF_LEN);
	memset(decode_window + REF_LEN, 0xa5, TARGET_LEN);
	decoded_len = crystal_lz4kd_decode_delta(encoded, decode_window,
			decode_window + REF_LEN, encoded_len, TARGET_LEN);
	KUNIT_ASSERT_EQ(test, decoded_len, TARGET_LEN);
	KUNIT_EXPECT_MEMEQ(test, decode_window + REF_LEN, target, TARGET_LEN);
}

static void crystal_sddc_lz4kd_delta_exact_reference_test(struct kunit *test)
{
	enum { REF_LEN = 31, TARGET_LEN = 31 };
	u8 *state = kunit_kmalloc(test,
			crystal_lz4kd_encode_state_bytes_min(), GFP_KERNEL);
	u8 *window = kunit_kmalloc(test, REF_LEN + TARGET_LEN, GFP_KERNEL);
	u8 *decode_window = kunit_kmalloc(test, REF_LEN + TARGET_LEN, GFP_KERNEL);
	u8 encoded[16];
	int encoded_len;
	int decoded_len;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, window);
	KUNIT_ASSERT_NOT_NULL(test, decode_window);
	crystal_sddc_fill_prandom(window, REF_LEN, 0x34f07591);
	memcpy(window + REF_LEN, window, TARGET_LEN);

	encoded_len = crystal_lz4kd_encode_delta(state, window,
			window + REF_LEN, encoded, TARGET_LEN, sizeof(encoded),
			sizeof(encoded));
	KUNIT_ASSERT_EQ(test, encoded_len, 1 + 3);
	memcpy(decode_window, window, REF_LEN);
	memset(decode_window + REF_LEN, 0xa5, TARGET_LEN);
	decoded_len = crystal_lz4kd_decode_delta(encoded, decode_window,
			decode_window + REF_LEN, encoded_len, TARGET_LEN);
	KUNIT_ASSERT_EQ(test, decoded_len, TARGET_LEN);
	KUNIT_EXPECT_MEMEQ(test, decode_window + REF_LEN, window, TARGET_LEN);
}

static void crystal_sddc_codec_roundtrip_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int encoded_len = 2 * PAGE_SIZE;
	unsigned int decoded_len = PAGE_SIZE;
	u8 *encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, encoded);
	KUNIT_ASSERT_NOT_NULL(test, decoded);
	KUNIT_ASSERT_NOT_NULL(test, page);
	memset(page, 0x5a, PAGE_SIZE);

	ret = crystal_sddc_codec_compress(ctx->codec, page, PAGE_SIZE,
					  encoded, &encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, encoded_len, (unsigned int)PAGE_SIZE);

	ret = crystal_sddc_codec_decompress(ctx->codec, encoded, encoded_len,
					    decoded, &decoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, decoded_len, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_MEMEQ(test, decoded, page, PAGE_SIZE);
}

static void crystal_sddc_codec_raw_contract_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int encoded_len = 2 * PAGE_SIZE;
	unsigned int decoded_len = PAGE_SIZE;
	u8 *encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, encoded);
	KUNIT_ASSERT_NOT_NULL(test, decoded);
	KUNIT_ASSERT_NOT_NULL(test, page);
	crystal_sddc_fill_prandom(page, PAGE_SIZE, 0x13579bdf);

	ret = crystal_sddc_codec_compress(ctx->codec, page, PAGE_SIZE,
					  encoded, &encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	if (encoded_len >= PAGE_SIZE) {
		KUNIT_EXPECT_EQ(test, encoded_len, 2U * PAGE_SIZE);
		return;
	}

	ret = crystal_sddc_codec_decompress(ctx->codec, encoded, encoded_len,
					    decoded, &decoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, decoded, page, PAGE_SIZE);
}

static void crystal_sddc_codec_delta_chain_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int ref_encoded_len = 2 * PAGE_SIZE;
	unsigned int cur_encoded_len = 2 * PAGE_SIZE;
	unsigned int delta_len = 2 * PAGE_SIZE;
	unsigned int restored_encoded_len = PAGE_SIZE;
	unsigned int restored_page_len = PAGE_SIZE;
	const void *restored_encoded;
	u8 *ref_encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *cur_encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *delta = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *restored_page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *ref_page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *cur_page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ref_encoded);
	KUNIT_ASSERT_NOT_NULL(test, cur_encoded);
	KUNIT_ASSERT_NOT_NULL(test, delta);
	KUNIT_ASSERT_NOT_NULL(test, restored_page);
	KUNIT_ASSERT_NOT_NULL(test, ref_page);
	KUNIT_ASSERT_NOT_NULL(test, cur_page);

	crystal_sddc_fill_prandom(ref_page, PAGE_SIZE / 2, 0x2468ace1);
	memcpy(ref_page + PAGE_SIZE / 2, ref_page, PAGE_SIZE / 2);
	memcpy(cur_page, ref_page, PAGE_SIZE);
	cur_page[127] ^= 0x55;
	cur_page[PAGE_SIZE / 2 + 127] ^= 0x55;

	ret = crystal_sddc_codec_compress(ctx->codec, ref_page, PAGE_SIZE,
					  ref_encoded, &ref_encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, ref_encoded_len, (unsigned int)PAGE_SIZE);
	ret = crystal_sddc_codec_compress(ctx->codec, cur_page, PAGE_SIZE,
					  cur_encoded, &cur_encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, cur_encoded_len, (unsigned int)PAGE_SIZE);

	ret = crystal_sddc_codec_compress_delta(ctx->codec, ref_encoded,
			ref_encoded_len, cur_encoded, cur_encoded_len, delta,
			&delta_len, cur_encoded_len - 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, delta_len, cur_encoded_len);

	ret = crystal_sddc_codec_decompress_delta_borrowed(ctx->codec, delta,
			delta_len, ref_encoded, ref_encoded_len, &restored_encoded,
			&restored_encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, restored_encoded);
	KUNIT_ASSERT_EQ(test, restored_encoded_len, cur_encoded_len);
	KUNIT_EXPECT_MEMEQ(test, restored_encoded, cur_encoded, cur_encoded_len);

	ret = crystal_sddc_codec_decompress(ctx->codec, restored_encoded,
			restored_encoded_len, restored_page, &restored_page_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, restored_page, cur_page, PAGE_SIZE);
}

static void crystal_sddc_codec_full_window_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int delta_len = 2 * PAGE_SIZE;
	unsigned int restored_len = PAGE_SIZE;
	u8 *ref = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *current_data = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *delta = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *restored = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ref);
	KUNIT_ASSERT_NOT_NULL(test, current_data);
	KUNIT_ASSERT_NOT_NULL(test, delta);
	KUNIT_ASSERT_NOT_NULL(test, restored);
	crystal_sddc_fill_prandom(ref, PAGE_SIZE, 0x10203040);
	memcpy(current_data, ref, PAGE_SIZE);
	current_data[0] ^= 0xa5;
	current_data[PAGE_SIZE - 1] ^= 0x5a;

	ret = crystal_sddc_codec_compress_delta(ctx->codec, ref, PAGE_SIZE,
			current_data, PAGE_SIZE, delta, &delta_len,
			PAGE_SIZE - 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, delta_len, (unsigned int)PAGE_SIZE);

	ret = crystal_sddc_codec_decompress_delta(ctx->codec, delta, delta_len,
			ref, PAGE_SIZE, restored, &restored_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, restored_len, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_MEMEQ(test, restored, current_data, PAGE_SIZE);
}

static void crystal_sddc_codec_malformed_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int encoded_len = 2 * PAGE_SIZE;
	unsigned int decoded_len = PAGE_SIZE;
	unsigned int restored_len = PAGE_SIZE;
	const void *restored = (const void *)1;
	u8 malformed[4] = { 0 };
	u8 *encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, encoded);
	KUNIT_ASSERT_NOT_NULL(test, decoded);
	KUNIT_ASSERT_NOT_NULL(test, page);
	memset(page, 0x6b, PAGE_SIZE);
	ret = crystal_sddc_codec_compress(ctx->codec, page, PAGE_SIZE,
					  encoded, &encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, 1U, encoded_len);
	ret = crystal_sddc_codec_decompress(ctx->codec, encoded,
			encoded_len - 1, decoded, &decoded_len);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	decoded_len = PAGE_SIZE;
	ret = crystal_sddc_codec_decompress(ctx->codec, malformed,
			sizeof(malformed), decoded, &decoded_len);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = crystal_sddc_codec_decompress_delta_borrowed(ctx->codec,
			malformed, sizeof(malformed), page, PAGE_SIZE, &restored,
			&restored_len);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_NULL(test, restored);
}

static void crystal_sddc_codec_delta_output_bounds_test(struct kunit *test)
{
	u8 *state = kunit_kmalloc(test,
			crystal_lz4kd_encode_state_bytes_min(), GFP_KERNEL);
	u8 *window = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 output[16];
	unsigned int out_size;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, window);
	memset(window, 0x5a, 2 * PAGE_SIZE);

	for (out_size = 0; out_size < ARRAY_SIZE(output); out_size++) {
		unsigned int i;

		memset(output, 0xa5, sizeof(output));
		ret = crystal_lz4kd_encode_delta(state, window,
				window + PAGE_SIZE, output, PAGE_SIZE, out_size,
				out_size);
		KUNIT_EXPECT_LE(test, ret, 0);
		for (i = out_size; i < ARRAY_SIZE(output); i++)
			KUNIT_EXPECT_EQ(test, output[i], (u8)0xa5);
	}
}

static struct kunit_case crystal_sddc_codec_test_cases[] = {
	KUNIT_CASE(crystal_sddc_lz4kd_small_offset_test),
	KUNIT_CASE(crystal_sddc_lz4kd_page_tail_match_test),
	KUNIT_CASE(crystal_sddc_lz4kd_delta_reference_start_test),
	KUNIT_CASE(crystal_sddc_lz4kd_delta_exact_reference_test),
	KUNIT_CASE(crystal_sddc_codec_roundtrip_test),
	KUNIT_CASE(crystal_sddc_codec_raw_contract_test),
	KUNIT_CASE(crystal_sddc_codec_delta_chain_test),
	KUNIT_CASE(crystal_sddc_codec_full_window_test),
	KUNIT_CASE(crystal_sddc_codec_malformed_test),
	KUNIT_CASE(crystal_sddc_codec_delta_output_bounds_test),
	{}
};

static struct kunit_suite crystal_sddc_codec_test_suite = {
	.name = "crystal-sddc-codec",
	.init = crystal_sddc_codec_test_init,
	.exit = crystal_sddc_codec_test_exit,
	.test_cases = crystal_sddc_codec_test_cases,
};

kunit_test_suite(crystal_sddc_codec_test_suite);
