#define PCRE2_CODE_UNIT_WIDTH 8

#include <pcre2.h>
#include <stdio.h>
#include <string.h>

#include "intervalRegexp.h"

struct compiled_range {
	pcre2_code *code;
	pcre2_match_data *match_data;
	char pattern[9000];
};

static int compile_range(struct compiled_range *range,
			 long min, long max, int flottant)
{
	char generated[8192];
	int errorcode;
	PCRE2_SIZE erroroffset;
	int written;

	if (regexpGen(generated, sizeof(generated), min, max, flottant) == 0) {
		fprintf(stderr, "regexpGen(%ld, %ld, %d) failed\n",
			min, max, flottant);
		return 1;
	}

	written = snprintf(range->pattern, sizeof(range->pattern), "^%s$",
			   generated);
	if ((written < 0) || ((size_t)written >= sizeof(range->pattern))) {
		fprintf(stderr, "generated regexp too large\n");
		return 1;
	}

	range->code = pcre2_compile((PCRE2_SPTR)range->pattern,
				    PCRE2_ZERO_TERMINATED, 0,
				    &errorcode, &erroroffset, NULL);
	if (!range->code) {
		PCRE2_UCHAR error[256];
		pcre2_get_error_message(errorcode, error, sizeof(error));
		fprintf(stderr, "pcre2_compile failed at %zu: %s\n",
			(size_t)erroroffset, (const char *)error);
		fprintf(stderr, "pattern: %s\n", range->pattern);
		return 1;
	}

	range->match_data = pcre2_match_data_create_from_pattern(range->code,
								 NULL);
	if (!range->match_data) {
		fprintf(stderr, "pcre2_match_data_create_from_pattern failed\n");
		pcre2_code_free(range->code);
		range->code = NULL;
		return 1;
	}

	return 0;
}

static void free_range(struct compiled_range *range)
{
	if (range->match_data) {
		pcre2_match_data_free(range->match_data);
		range->match_data = NULL;
	}
	if (range->code) {
		pcre2_code_free(range->code);
		range->code = NULL;
	}
}

static int range_matches(struct compiled_range *range, const char *text)
{
	int rc = pcre2_match(range->code, (PCRE2_SPTR)text, strlen(text),
			    0, 0, range->match_data, NULL);
	return rc >= 0;
}

static int check_integer_range(long min, long max,
			       long probe_min, long probe_max)
{
	struct compiled_range range = {0};
	long low = min < max ? min : max;
	long high = min < max ? max : min;
	long value;
	int failed = 0;

	if (compile_range(&range, min, max, 0) != 0)
		return 1;

	for (value = probe_min; value <= probe_max; value++) {
		char text[64];
		int expected = (value >= low) && (value <= high);
		int got;

		snprintf(text, sizeof(text), "%ld", value);
		got = range_matches(&range, text);
		if (got != expected) {
			fprintf(stderr,
				"range %ld..%ld: value %s expected %d got %d\n",
				min, max, text, expected, got);
			fprintf(stderr, "pattern: %s\n", range.pattern);
			failed = 1;
			break;
		}
	}

	free_range(&range);
	return failed;
}

static int check_sample(long min, long max, int flottant,
			const char *text, int expected)
{
	struct compiled_range range = {0};
	int got;

	if (compile_range(&range, min, max, flottant) != 0)
		return 1;

	got = range_matches(&range, text);
	if (got != expected) {
		fprintf(stderr,
			"range %ld..%ld flottant=%d: text '%s' expected %d got %d\n",
			min, max, flottant, text, expected, got);
		fprintf(stderr, "pattern: %s\n", range.pattern);
		free_range(&range);
		return 1;
	}

	free_range(&range);
	return 0;
}

int main(void)
{
	int failed = 0;

	failed |= check_integer_range(0, 9, -2, 11);
	failed |= check_integer_range(8, 12, 5, 15);
	failed |= check_integer_range(12, 8, 5, 15);
	failed |= check_integer_range(98, 102, 95, 105);
	failed |= check_integer_range(990, 1010, 985, 1015);
	failed |= check_integer_range(-10, -5, -12, -3);
	failed |= check_integer_range(-3, 3, -5, 5);

	failed |= check_sample(1, 3, 1, "1", 1);
	failed |= check_sample(1, 3, 1, "1.5", 1);
	failed |= check_sample(1, 3, 1, "2.25", 1);
	failed |= check_sample(1, 3, 1, "3.0", 1);
	failed |= check_sample(1, 3, 1, "0.9", 0);
	failed |= check_sample(1, 3, 1, "3.1", 0);
	failed |= check_sample(-3, -1, 1, "-2.5", 1);
	failed |= check_sample(-3, -1, 1, "-3.5", 0);
	failed |= check_sample(-1, 1, 1, "-0.5", 1);
	failed |= check_sample(-1, 1, 1, "1.1", 0);

	return failed ? 1 : 0;
}
