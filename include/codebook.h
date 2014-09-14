#ifndef _CODEBOOK_H_
#define _CODEBOOK_H_
/**
 * Functions and definitions relating to reading codebooks from files, used
 * for both the encoder and decoder code
 */

#include "util.h"

#include <stdint.h>
#include <string.h>

#include "well.h"
#include "pmf.h"
#include "distortion.h"
#include "quantizer.h"
#include "lines.h"

/**
 * Stores an array of conditional PMFs for the current column given the previous
 * column. PMF pointers are stored in a flat array so don't try to find the PMF you
 * want directly--use the accessor
 */
struct cond_pmf_list_t {
	uint32_t columns;
	const struct alphabet_t *alphabet;
	struct pmf_t **pmfs;
	struct pmf_list_t *marginal_pmfs;
};

/**
 * Stores an array of quantizer pointers for the column for all possible left context
 * values. Unused ones are left as null pointers. This is also stored as a flat array
 * so the accessor must be used to look up the correct quantizer
 * The dreaded triple pointer is used to store an array of (different length) arrays
 * of pointers to quantizers
 */
struct cond_quantizer_list_t {
	uint32_t columns;
	struct alphabet_t **input_alphabets;
	struct quantizer_t ***q;
	double **ratio;				// Raw ratio
	uint8_t **qratio;			// Quantized ratio
	struct well_state_t well;
};

// Memory management
struct cond_pmf_list_t *alloc_conditional_pmf_list(const struct alphabet_t *alphabet, uint32_t columns);
struct cond_quantizer_list_t *alloc_conditional_quantizer_list(uint32_t columns);
void free_cond_pmf_list(struct cond_pmf_list_t *);
void free_cond_quantizer_list(struct cond_quantizer_list_t *);

// Per-column initializer for conditional quantizer list
void cond_quantizer_init_column(struct cond_quantizer_list_t *list, uint32_t column, const struct alphabet_t *input_union);

// Accessors
struct pmf_t *get_cond_pmf(struct cond_pmf_list_t *list, uint32_t column, symbol_t prev);
struct quantizer_t *get_cond_quantizer_indexed(struct cond_quantizer_list_t *list, uint32_t column, uint32_t index);
struct quantizer_t *get_cond_quantizer(struct cond_quantizer_list_t *list, uint32_t column, symbol_t prev);
void store_cond_quantizers(struct quantizer_t *restrict lo, struct quantizer_t *restrict hi, double ratio, struct cond_quantizer_list_t *list, uint32_t column, symbol_t prev);
void store_cond_quantizers_indexed(struct quantizer_t *restrict lo, struct quantizer_t *restrict hi, double ratio, struct cond_quantizer_list_t *list, uint32_t column, uint32_t index);
void store_single_quantizer_indexed(struct quantizer_t *q, struct cond_quantizer_list_t *list, uint32_t column, uint32_t index);
struct quantizer_t *choose_quantizer(struct cond_quantizer_list_t *list, uint32_t column, symbol_t prev);
uint32_t find_state_encoding(struct quantizer_t *codebook, symbol_t value);

// Meat of the implementation
void calculate_statistics(struct quality_file_t *, struct cond_pmf_list_t *);
double optimize_for_entropy(struct pmf_t *pmf, struct distortion_t *dist, double target, struct quantizer_t **lo, struct quantizer_t **hi);
struct cond_quantizer_list_t *generate_codebooks(struct quality_file_t *info, struct cond_pmf_list_t *in_pmfs, struct distortion_t *dist, double comp, double *expected_distortion);
struct cond_quantizer_list_t *generate_codebooks_greg(struct quality_file_t *info, struct cond_pmf_list_t *in_pmfs, struct distortion_t *dist, double comp, uint32_t mode, double *expected_distortion);

// Legacy stuff to be converted still

#define MAX_CODEBOOK_LINE_LENGTH 4096

struct codebook_t {
	uint8_t *quantizer;				// Array of quantizer mapping (index is input)
	uint8_t *uniques;				// Array of unique values in quantizer list
	uint8_t max_unique_count;		// Maximum number of uniques allowed. Unused?
	uint8_t actual_unique_count;	// Actual number of unique elements
	uint8_t bits;					// Number of bits used for state encoding this codebook
	uint8_t symbols;				// Possible number of symbols in alphabet (length of quantizer)
};

struct codebook_list_t {
	struct codebook_t **high;
	struct codebook_t **low;
	uint8_t *ratio;
	uint32_t *select_count;
	uint8_t symbols;
	uint32_t columns;
	struct well_state_t well;
};

#define COPY_Q_TO_LINE(line, q, i, size) for (i = 0; i < size; ++i) { line[i] = q[i] + 33; }
#define COPY_Q_FROM_LINE(line, q, i, size) for (i = 0; i < size; ++i) { q[i] = line[i] - 33; }

// Master function to read a codebook from a file
void write_codebook(const char *filename, struct cond_quantizer_list_t *quantizers);
struct cond_quantizer_list_t *read_codebook(const char *filename, const struct alphabet_t *A);

// Initialization and parsing
void init_codebook_list(struct codebook_list_t *list, uint8_t symbols, uint32_t columns);
void init_codebook_array(struct codebook_t **cb, uint8_t symbols, uint32_t columns);
void generate_uniques(struct codebook_t *cb);

// Operational functions
struct codebook_t *choose_codebook(struct codebook_list_t *list, uint32_t column, uint8_t prev_value);

// Debugging
void print_codebook(struct codebook_t *cb);

#endif
