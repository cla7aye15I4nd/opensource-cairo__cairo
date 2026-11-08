/*
 * Copyright © 2026 Ana Bobillo
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Author: Ana Bobillo <anabobillo3@gmail.com>
 */

#include "cairo-test.h"

#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <cairo-pdf.h>

/* Check that the PDF surface does not embed JPEG, JPEG 2000 or JBIG2
 * mime data whose header is invalid.
 *
 * Run under AddressSanitizer, this also checks that the header
 * parsers stay within the data.
 */

typedef struct _mime_test {
    const char *name;
    const char *mime_type;
    const char *filter;		/* PDF filter of the embedded data */
    cairo_bool_t embedded;	/* whether the data is embedded */
    const unsigned char *data;
    unsigned long length;	/* bytes attached as mime data */
} mime_test_t;

/* A start of frame segment. jpeg-truncated-sof attaches it without
 * its last byte, the number of components, which stays in the buffer:
 * reading past the end of the mime data embeds the image. */
static const unsigned char jpeg_sof[] = {
    0xff, 0xd8,				/* SOI */
    0xff, 0xc0, 0x00, 0x11, 0x08,	/* SOF0, length, precision */
    0x00, 0x01, 0x00, 0x01,		/* height, width */
    0x03				/* number of components */
};

/* No start of frame segment. */
static const unsigned char jpeg_no_sof[] = {
    0xff, 0xd8,				/* SOI */
    0xff, 0xd9				/* EOI */
};

/* A segment longer than the data, so no start of frame segment. */
static const unsigned char jpeg_long_segment[] = {
    0xff, 0xd8,				/* SOI */
    0xff, 0xe0, 0xff, 0xff,		/* APP0, length */
    0x00, 0x00
};

/* A box with a length of 0 before the JP2 header box. */
static const unsigned char jpx_zero_length_box[] = {
    0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20,	/* signature box */
    0x0d, 0x0a, 0x87, 0x0a,
    0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70,	/* file type box */
    0x6a, 0x70, 0x32, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x6a, 0x70, 0x32, 0x20,
    0x00, 0x00, 0x00, 0x00, 0x78, 0x6d, 0x6c, 0x20,	/* XML box, length 0 */
    0x00, 0x00, 0x00, 0x00
};

/* A generic region segment of unknown length whose data ends with
 * the first byte of the end sequence. */
static const unsigned char jbig2_unterminated_region[] = {
    0x00, 0x00, 0x00, 0x00,		/* segment number */
    0x26,				/* immediate generic region */
    0x00,				/* no referred-to segments */
    0x01,				/* page association */
    0xff, 0xff, 0xff, 0xff,		/* data length unknown */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* region segment information */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,				/* generic region flags */
    0xff
};

#define MIME_TEST(name, type, filter, embedded, data, length) \
    { name, type, filter, embedded, data, length }

static const mime_test_t mime_tests[] = {
    MIME_TEST ("jpeg-sof",
	       CAIRO_MIME_TYPE_JPEG, "/DCTDecode", TRUE,
	       jpeg_sof, sizeof (jpeg_sof)),
    MIME_TEST ("jpeg-truncated-sof",
	       CAIRO_MIME_TYPE_JPEG, "/DCTDecode", FALSE,
	       jpeg_sof, sizeof (jpeg_sof) - 1),
    MIME_TEST ("jpeg-no-sof",
	       CAIRO_MIME_TYPE_JPEG, "/DCTDecode", FALSE,
	       jpeg_no_sof, sizeof (jpeg_no_sof)),
    MIME_TEST ("jpeg-long-segment",
	       CAIRO_MIME_TYPE_JPEG, "/DCTDecode", FALSE,
	       jpeg_long_segment, sizeof (jpeg_long_segment)),
    MIME_TEST ("jpx-zero-length-box",
	       CAIRO_MIME_TYPE_JP2, "/JPXDecode", FALSE,
	       jpx_zero_length_box, sizeof (jpx_zero_length_box)),
    MIME_TEST ("jbig2-unterminated-region",
	       CAIRO_MIME_TYPE_JBIG2, "/JBIG2Decode", FALSE,
	       jbig2_unterminated_region, sizeof (jbig2_unterminated_region)),
};

typedef struct _buffer {
    unsigned char *data;
    size_t length;
    size_t size;
} buffer_t;

static cairo_status_t
write_func (void *closure, const unsigned char *data, unsigned int length)
{
    buffer_t *buffer = closure;

    if (buffer->length + length > buffer->size) {
	size_t size = 2 * (buffer->length + length);
	unsigned char *new_data = realloc (buffer->data, size);

	if (new_data == NULL)
	    return CAIRO_STATUS_NO_MEMORY;

	buffer->data = new_data;
	buffer->size = size;
    }

    memcpy (buffer->data + buffer->length, data, length);
    buffer->length += length;

    return CAIRO_STATUS_SUCCESS;
}

static cairo_bool_t
buffer_contains (const buffer_t *buffer, const char *str)
{
    size_t len = strlen (str);
    size_t i;

    for (i = 0; i + len <= buffer->length; i++) {
	if (memcmp (buffer->data + i, str, len) == 0)
	    return TRUE;
    }

    return FALSE;
}

static cairo_test_status_t
test_mime_data (const cairo_test_context_t *ctx, const mime_test_t *test)
{
    cairo_surface_t *image;
    cairo_surface_t *surface;
    cairo_t *cr;
    cairo_status_t status;
    buffer_t buffer = { NULL, 0, 0 };
    cairo_test_status_t result = CAIRO_TEST_SUCCESS;

    image = cairo_image_surface_create (CAIRO_FORMAT_RGB24, 1, 1);

    status = cairo_surface_set_mime_data (image, test->mime_type,
					  test->data, test->length,
					  NULL, NULL);
    if (status) {
	cairo_surface_destroy (image);
	cairo_test_log (ctx, "%s: failed to set mime data: %s\n",
			test->name, cairo_status_to_string (status));
	return CAIRO_TEST_FAILURE;
    }

    surface = cairo_pdf_surface_create_for_stream (write_func, &buffer, 1, 1);
    cr = cairo_create (surface);
    cairo_set_source_surface (cr, image, 0, 0);
    cairo_paint (cr);
    status = cairo_status (cr);
    cairo_destroy (cr);
    cairo_surface_finish (surface);
    if (status == CAIRO_STATUS_SUCCESS)
	status = cairo_surface_status (surface);
    cairo_surface_destroy (surface);
    cairo_surface_destroy (image);

    if (status) {
	cairo_test_log (ctx, "%s: failed to create pdf: %s\n",
			test->name, cairo_status_to_string (status));
	result = CAIRO_TEST_FAILURE;
    } else if (buffer_contains (&buffer, test->filter) != test->embedded) {
	cairo_test_log (ctx, "%s: mime data %s with %s\n",
			test->name,
			test->embedded ? "not embedded" : "embedded",
			test->filter);
	result = CAIRO_TEST_FAILURE;
    }

    free (buffer.data);

    return result;
}

static cairo_test_status_t
preamble (cairo_test_context_t *ctx)
{
    cairo_test_status_t result = CAIRO_TEST_SUCCESS;
    int i;

    if (! cairo_test_is_target_enabled (ctx, "pdf"))
	return CAIRO_TEST_UNTESTED;

    for (i = 0; i < ARRAY_LENGTH (mime_tests); i++) {
	if (test_mime_data (ctx, &mime_tests[i]) != CAIRO_TEST_SUCCESS)
	    result = CAIRO_TEST_FAILURE;
    }

    return result;
}

CAIRO_TEST (pdf_mime_data_invalid,
	    "Check invalid mime data is not embedded by the PDF surface",
	    "pdf, mime-data", /* keywords */
	    NULL, /* requirements */
	    0, 0,
	    preamble, NULL)
