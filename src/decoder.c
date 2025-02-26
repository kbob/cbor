#include "cbor/cbor.h"
#include <stdbool.h>

#define INDEFINITE_VALUE			((uint8_t)-1)
#define RESERVED_VALUE				((uint8_t)-2)

#define ADDITIONAL_INFO_MASK			0x1fu /* the low-order 5 bits */

#define get_major_type(data_item)		((data_item) >> 5)
#define get_additional_info(major_type)		\
	((major_type) & ADDITIONAL_INFO_MASK)

#if !defined(MIN)
#define MIN(a, b)				(((a) > (b))? (b) : (a))
#endif
#if !defined(assert)
#define assert(expr)
#endif

struct decode_context {
	uint8_t *buf;
	size_t bufsize;
	size_t bufidx;

	const uint8_t *msg;
	size_t msgsize;
	size_t msgidx;

	uint8_t major_type;
	uint8_t additional_info;
	uint8_t following_bytes;

	uint8_t recursion_depth;
	_Static_assert(CBOR_RECURSION_MAX_LEVEL < 256, "");
};

typedef cbor_error_t (*major_type_callback_t)(struct decode_context *ctx);

static cbor_error_t do_unsigned_integer(struct decode_context *ctx);
static cbor_error_t do_negative_integer(struct decode_context *ctx);
static cbor_error_t do_string(struct decode_context *ctx);
static cbor_error_t do_recursive(struct decode_context *ctx);
static cbor_error_t do_tag(struct decode_context *ctx);
static cbor_error_t do_float_and_other(struct decode_context *ctx);

/* 8 callbacks for 3-bit major type */
static const major_type_callback_t callbacks[] = {
	do_unsigned_integer,	/* 0 */
	do_negative_integer,	/* 1 */
	do_string,		/* 2: byte string */
	do_string,		/* 3: text string encoded as utf-8 */
	do_recursive,		/* 4: array */
	do_recursive,		/* 5: map */
	do_tag,			/* 6 */
	do_float_and_other,	/* 7 */
};
_Static_assert(sizeof(callbacks) == 8 * sizeof(major_type_callback_t), "");

static void copy_data_le(uint8_t *dst, const uint8_t *src, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		dst[len - i - 1] = src[i];
	}
}

static void copy_data_be(uint8_t *dst, const uint8_t *src, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		dst[i] = src[i];
	}
}
static void copy_data(uint8_t *dst, const uint8_t *src, size_t len)
{
#if defined(CBOR_BIG_ENDIAN)
	copy_data_be(dst, src, len);
#else
	copy_data_le(dst, src, len);
#endif
}

static uint8_t get_following_bytes(uint8_t additional_info)
{
	if (additional_info < 24) {
		return 0;
	} else if (additional_info == 31) {
		return INDEFINITE_VALUE;
	} else if (additional_info >= 28) {
		return RESERVED_VALUE;
	}

	return (uint8_t)(1u << (additional_info - 24));
}

static bool has_valid_buffers_for_following_bytes(
		const struct decode_context *ctx, cbor_error_t *err)
{
	if (ctx->following_bytes == RESERVED_VALUE) {
		*err = CBOR_ILLEGAL;
		return false;
	} else if (ctx->following_bytes == INDEFINITE_VALUE) {
		return true;
	}

	if ((ctx->following_bytes + 1u) > ctx->msgsize - ctx->msgidx) {
		*err = CBOR_ILLEGAL;
		return false;
	}
	if (ctx->following_bytes > ctx->bufsize - ctx->bufidx) {
		*err = CBOR_UNDERRUN;
		return false;
	}

	return true;
}

static cbor_error_t decode_cbor(struct decode_context *ctx, size_t maxitem)
{
	cbor_error_t err = CBOR_SUCCESS;
	size_t item = 0;

	if (++ctx->recursion_depth > CBOR_RECURSION_MAX_LEVEL) {
		return CBOR_EXCESSIVE;
	}

	while (ctx->msgidx < ctx->msgsize && ctx->bufidx < ctx->bufsize) {
		ctx->major_type = get_major_type(ctx->msg[ctx->msgidx]);
		ctx->additional_info = get_additional_info(ctx->msg[ctx->msgidx]);
		ctx->following_bytes = get_following_bytes(ctx->additional_info);

		if (!has_valid_buffers_for_following_bytes(ctx, &err)) {
			break;
		}

		err = callbacks[ctx->major_type](ctx);

		if (err == CBOR_BREAK) {
			if (ctx->msgidx == ctx->msgsize) {
				break;
			}
		} else if (err != CBOR_SUCCESS) {
			break;
		}

		item += 1;
		err = CBOR_SUCCESS;

		if (item >= maxitem && maxitem > 0) {
			break;
		}
	}

	ctx->recursion_depth--;

	assert(ctx->msgidx <= ctx->msgsize);
	assert(ctx->bufidx <= ctx->bufsize);

	return err;
}

static cbor_error_t do_unsigned_integer(struct decode_context *ctx)
{
	const uint8_t *msg = &ctx->msg[ctx->msgidx];
	uint8_t *buf = &ctx->buf[ctx->bufidx];

	if (ctx->following_bytes == INDEFINITE_VALUE) {
		return CBOR_ILLEGAL;
	} else if (ctx->following_bytes == 0) {
		*buf = ctx->additional_info;
		ctx->bufidx++;
	}

	copy_data(buf, &msg[1], ctx->following_bytes);

	ctx->bufidx += ctx->following_bytes;
	ctx->msgidx += ctx->following_bytes + 1;

	return CBOR_SUCCESS;
}

static cbor_error_t do_negative_integer(struct decode_context *ctx)
{
	size_t buf_start_index = ctx->bufidx;
	uint8_t *buf = &ctx->buf[buf_start_index];

	cbor_error_t err = do_unsigned_integer(ctx);

	if (err != CBOR_SUCCESS) {
		return err;
	}

	uint8_t data_size = (uint8_t)(ctx->bufidx - buf_start_index);
	uint8_t data_type_size = data_size;
	uint8_t msb_bit = (uint8_t)(data_size * 8 - 1);
	assert(data_type_size <= sizeof(uint64_t));
	assert(msb_bit < sizeof(uint64_t) * 8);

	uint64_t val = 0;
	copy_data_be((uint8_t *)&val, buf, data_size);

	/* expand data type size if needed */
	if (data_type_size < sizeof(uint64_t) && val & (1lu << msb_bit)) {
		data_type_size <<= 1;
		ctx->bufidx += data_type_size - data_size;
	}

	val = ~val;

	/* The value becomes a positive value if the data type size of the
	 * variable is larger than the value size. So we set MSB first here to
	 * keep it negative. */
	for (uint8_t i = 0; i < MIN(ctx->bufsize - buf_start_index, 4u); i++) {
		buf[i] = 0xff;
	}

	copy_data_be(buf, (uint8_t *)&val, data_size);

	return CBOR_SUCCESS;
}

static size_t go_get_item_length(struct decode_context *ctx)
{
	const uint8_t *msg = &ctx->msg[ctx->msgidx];
	uint64_t len = 0;
	size_t offset = 0;

	if (ctx->following_bytes == INDEFINITE_VALUE) {
		/* no need to do anything */
	} else if (ctx->following_bytes == 0) {
		len = ctx->additional_info;
	} else {
		copy_data((uint8_t *)&len, &msg[1], ctx->following_bytes);
		offset = ctx->following_bytes;
	}

	ctx->msgidx += offset + 1;

	return (size_t)len;
}

static cbor_error_t do_string(struct decode_context *ctx)
{
	size_t len = go_get_item_length(ctx);
	const uint8_t *msg = &ctx->msg[ctx->msgidx];
	uint8_t *buf = &ctx->buf[ctx->bufidx];

	size_t maxlen =
		MIN(ctx->bufsize - ctx->bufidx, ctx->msgsize - ctx->msgidx);

	if (len > maxlen) {
		return CBOR_ILLEGAL;
	}

	for (size_t i = 0; i < len; i++) {
		buf[i] = msg[i];
	}

	ctx->bufidx += len;
	ctx->msgidx += len;

	return CBOR_SUCCESS;
}

static cbor_error_t do_recursive(struct decode_context *ctx)
{
	return decode_cbor(ctx, go_get_item_length(ctx));
}

/* TODO: Implement tag */
static cbor_error_t do_tag(struct decode_context *ctx)
{
	(void)ctx;
	return CBOR_INVALID;
}

static uint8_t get_simple_value(uint8_t val)
{
	switch (val) {
	case 20: /* false */
		return 0;
	case 21: /* true */
		return 1;
	case 22: /* null */
		return '\0';
	case 23: /* undefined */
	default:
		return val;
	}
}

static cbor_error_t do_float_and_other(struct decode_context *ctx)
{
	cbor_error_t err;

	if (ctx->following_bytes == INDEFINITE_VALUE) {
		ctx->msgidx++;
		return CBOR_BREAK;
	} else if (ctx->following_bytes == 1) {
		ctx->following_bytes = 0;
		ctx->additional_info = ctx->msg[++ctx->msgidx];
	}

	if (!has_valid_buffers_for_following_bytes(ctx, &err)) {
		return err;
	}

	err = do_unsigned_integer(ctx);

	if (ctx->following_bytes == 0) {
		ctx->buf[ctx->bufidx - 1] =
			get_simple_value(ctx->buf[ctx->bufidx - 1]);
	}

	return err;
}

cbor_error_t cbor_decode(void *buf, size_t bufsize,
		const uint8_t *msg, size_t msgsize)
{
	struct decode_context ctx = {
		.buf = (uint8_t *)buf,
		.bufsize = bufsize,
		.bufidx = 0,
		.msg = msg,
		.msgsize = msgsize,
		.msgidx = 0,
	};

	cbor_error_t err = decode_cbor(&ctx, 0);

	if (err == CBOR_SUCCESS) {
		if (ctx.msgidx < ctx.msgsize) {
			err = CBOR_OVERRUN;
		} else if (ctx.bufidx < ctx.bufsize) {
			err = CBOR_UNDERRUN;
		}
	}

	return err;
}
