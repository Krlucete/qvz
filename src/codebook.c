#include "codebook.h"
#include "lines.h"

#include <stdio.h>
#include <assert.h>

/**
 * To compute stats for the training data, we will need a set of conditional PMFs, one
 * per column
 * @param alphabet The symbol alphabet for each column
 * @param columns The number of columns to allocate conditional PMFs for
 */
struct cond_pmf_list_t *alloc_conditional_pmf_list(const struct alphabet_t *alphabet, uint32_t columns) {
	uint32_t count = 1 + alphabet->size*(columns-1);
	uint32_t i;
	struct cond_pmf_list_t *list = (struct cond_pmf_list_t *) calloc(1, sizeof(struct cond_pmf_list_t));

	// We need one array of PMF pointers that will index into the buffer allocated above, for the columns
	list->columns = columns;
	list->alphabet = alphabet;
	list->pmfs = (struct pmf_t **) calloc(count, sizeof(struct pmf_t *));

	// All PMFs are stored in a flat array, the accessor function will resolve a PMF's address
	for (i = 0; i < count; ++i) {
		list->pmfs[i] = alloc_pmf(alphabet);
	}

	return list;
}

/**
 * Deallocate the PMF list given and unallocate the two allocated memory blocks
 * @param list The conditional pmf list to deallocate
 */
void free_conditional_pmf_list(struct cond_pmf_list_t *list) {
	uint32_t count = 1 + list->alphabet->size * (list->columns - 1);
	uint32_t i;

	for (i = 0; i < count; ++i) {
		free_pmf(list->pmfs[0]);
	}
	free(list);

	free_pmf_list(list->marginal_pmfs);
}

/**
 * Allocate the quantizer list structure and the first level of array based on columns
 * @param columns The number of columns for which we have quantizers
 * @return Pointer to conditional quantizer list structure
 */
struct cond_quantizer_list_t *alloc_conditional_quantizer_list(uint32_t columns) {
	struct cond_quantizer_list_t *rtn = (struct cond_quantizer_list_t *) calloc(1, sizeof(struct cond_quantizer_list_t));
	rtn->columns = columns;
	rtn->input_alphabets = (struct alphabet_t **) calloc(columns, sizeof(struct alphabet_t *));
	rtn->q = (struct quantizer_t ***) calloc(columns, sizeof(struct quantizer_t **));
    rtn->ratio = (double **) calloc(columns, sizeof(double *));
	return rtn;
}

/**
 * Deallocate the quantizer list as well as any alphabets or pmfs that are stored
 * @param list The conditional quantizer list to deallocate
 */
void free_cond_quantizer_list(struct cond_quantizer_list_t *list) {
	uint32_t i, j;

	for (i = 0; i < list->columns; ++i) {
		if (list->q[i]) {
			for (j = 0; j < list->input_alphabets[i]->size; ++i) {
				if (list->q[i][j])
					free_quantizer(list->q[i][j]);
			}
			free_alphabet(list->input_alphabets[i]);
			free(list->q[i]);
			free(list->ratio[i]);
		}
	}

	free(list->ratio);
	free(list->q);
	free(list->input_alphabets);
	free(list);
}

/**
 * Initialize the information within a quantizer for the given column. This can't be done
 * at allocation time because we don't know everything about this column until we get here
 * during the optimization process
 * @param list The conditional quantizer list to update
 * @param column The column to initialize
 * @param input_union The alphabet of all possible left context symbols
 */
void cond_quantizer_init_column(struct cond_quantizer_list_t *list, uint32_t column, const struct alphabet_t *input_union) {
	list->input_alphabets[column] = duplicate_alphabet(input_union);
	// Low and high quantizer per element of the input union
	list->q[column] = (struct quantizer_t **) calloc(input_union->size*2, sizeof(struct quantizer_t *));
	// One ratio per element of input union
	list->ratio[column] = (double *) calloc(input_union->size, sizeof(double));
}

/**
 * Find a PMF for a specific column with the specific previous value
 */
struct pmf_t *get_cond_pmf(struct cond_pmf_list_t *list, uint32_t column, symbol_t prev) {
	if (column == 0)
		return list->pmfs[0];
	return list->pmfs[1 + (column-1)*list->alphabet->size + prev];
}

/**
 * Get a quantizer by its indexed location within the quantizer list for a column
 */
struct quantizer_t *get_cond_quantizer_indexed(struct cond_quantizer_list_t *list, uint32_t column, uint32_t index) {
	return list->q[column][index];
}

/**
 * Get a quantizer by its left context symbol
 */
struct quantizer_t *get_cond_quantizer(struct cond_quantizer_list_t *list, uint32_t column, symbol_t prev) {
	uint32_t idx = get_symbol_index(list->input_alphabets[column], prev);
	if (idx != ALPHABET_SYMBOL_NOT_FOUND)
		return get_cond_quantizer_indexed(list, column, idx);
	return NULL;
}

/**
 * Stores the given quantizer at the appropriate index corresponding to the left context symbol given
 * for the specific column
 */
void store_cond_quantizers(struct quantizer_t *restrict lo, struct quantizer_t *restrict hi, struct cond_quantizer_list_t *list, uint32_t column, symbol_t prev) {
	uint32_t idx = get_symbol_index(list->input_alphabets[column], prev);
	list->q[column][2*idx] = lo;
	list->q[column][2*idx + 1] = hi;
    list->ratio[column][idx] = lo->ratio;
}

/**
 * Selects a quantizer for the given column from the quantizer list with the appropriate ratio
 */
struct quantizer_t *choose_quantizer(struct cond_quantizer_list_t *list, uint32_t column, symbol_t prev) {
	uint32_t idx = get_symbol_index(list->input_alphabets[column], prev);
	assert(idx != ALPHABET_SYMBOL_NOT_FOUND);
	if (well_1024a(&list->well) / ((double)UINT32_MAX) >= list->ratio[column][idx]) {
		return list->q[column][2*idx+1];
	}
	return list->q[column][2*idx];
}

/**
 * Converts a quality score into a state encoded value, which is the same as doing a symbol index lookup
 * in the output alphabet. This needs to be inlined.
 */
uint32_t find_state_encoding(struct quantizer_t *q, symbol_t value) {
	return get_symbol_index(q->output_alphabet, value);
}

/**
 * Given a quality info struct, which is assumed to have loaded the training set, and a
 * set of output conditional pmf structures, calculate the statistics of the data
 */
void calculate_statistics(struct quality_file_t *info, struct cond_pmf_list_t *pmf_list) {
	uint32_t block_idx, line_idx, column;
	uint32_t j;
	struct line_t *line;

	// Calculate all conditional PMFs
	for (block_idx = 0; block_idx < info->block_count; ++block_idx) {
		for (line_idx = 0; line_idx < info->blocks[block_idx].count; ++line_idx) {
			line = &info->blocks[block_idx].lines[line_idx];
			pmf_increment(get_cond_pmf(pmf_list, 0, 0), line->data[0]);
			for (column = 1; column < info->columns; ++column) {
				pmf_increment(get_cond_pmf(pmf_list, column, line->data[column-1]), line->data[column]);
			}
		}
	}

	// Calculate unconditional PMFs afterwards
	pmf_list->marginal_pmfs = alloc_pmf_list(info->columns, pmf_list->alphabet);
	combine_pmfs(get_cond_pmf(pmf_list, 0, 0), pmf_list->marginal_pmfs->pmfs[0], 1.0, 0.0, pmf_list->marginal_pmfs->pmfs[0]);
	for (column = 1; column < info->columns; ++column) {
		for (j = 0; j < pmf_list->alphabet->size; ++j) {
			combine_pmfs(pmf_list->marginal_pmfs->pmfs[column], get_cond_pmf(pmf_list, column, j), 1.0, get_probability(pmf_list->marginal_pmfs->pmfs[column-1], j), pmf_list->marginal_pmfs->pmfs[column]);
		}
	}
}

/**
 * Calculates the integer number of states to use for each column according to the estimate
 * of conditional entropy from the baseline statistics
 * @deprecated will be removed as soon as the new version is working
 */
void find_bit_allocation(struct cond_pmf_list_t *pmf_list, double comp, uint32_t **high_out, uint32_t **low_out, double **ratio_out, uint32_t mode) {
	uint32_t *high = (uint32_t *) calloc(pmf_list->columns, sizeof(uint32_t));
	uint32_t *low = (uint32_t *) calloc(pmf_list->columns, sizeof(uint32_t));
	double *ratio = (double *) calloc(pmf_list->columns, sizeof(double));
	double *entropies = (double *) _alloca(pmf_list->columns*sizeof(double));
	double h_lo, h_hi;
	uint32_t i, j;
	struct pmf_list_t *uc_pmf_list = alloc_pmf_list(pmf_list->columns, pmf_list->alphabet);
	struct pmf_t *pmf_temp;

	*high_out = high;
	*low_out = low;
	*ratio_out = ratio;

	// Column 0 pmf is the unconditional one, copy it to the unconditional pmf list
	pmf_temp = get_cond_pmf(pmf_list, 0, 0);
	combine_pmfs(pmf_temp, pmf_temp, 1.0, 0.0, uc_pmf_list->pmfs[0]);

	// Find unconditional pmfs for each column remaining
	for (i = 1; i < pmf_list->columns; ++i) {
		for (j = 0; j < pmf_list->alphabet->size; ++j) {
			pmf_temp = get_cond_pmf(pmf_list, i, j);
			combine_pmfs(uc_pmf_list->pmfs[i], pmf_temp, 1.0, get_probability(uc_pmf_list->pmfs[i-1], j), uc_pmf_list->pmfs[i]);
		}
	}

	// Alloca doesn't zero memory for us
	memset(entropies, 0, pmf_list->columns*sizeof(double));

	// Column 0 is handled specially because it only has one left context
	entropies[0] = get_entropy(get_cond_pmf(pmf_list, 0, 0)) * comp;

	// Rest of the columns are handled identically
	for (i = 1; i < pmf_list->columns; ++i) {
		pmf_temp = uc_pmf_list->pmfs[i-1];
		for (j = 0; j < pmf_list->alphabet->size; ++j) {
			entropies[i] += get_probability(pmf_temp, j) * get_entropy(get_cond_pmf(pmf_list, i, j));
		}
		entropies[i] = entropies[i] * comp;
	}
	
	// MIKEL: The conditional entropies are computed.
	// MIKEL: Instead of using the average entropy (entropy[i]), we need to use the individual conditional entropies.
	// MIKEL: If I'm not mistaken, "get_entropy(get_cond_pmf(pmf_list, i, j))" is exactly what we need. 
	// MIKEL: So we need to change from entropies[i] to entropies[i][j].
	
	// Compute number of states used based on mode parameter
	// r = ratio:
	// H = rH_lo + (1-r)H_hi
	// H - H_hi = r(H_lo - H_hi)
	// r = (H - H_hi) / (H_lo - H_hi)
	for (i = 0; i < pmf_list->columns; ++i) {
		switch(mode) {
			case BIT_ALLOC_MODE_INT_STATES:
				h_lo = pow(2, entropies[i]);
				low[i] = (uint32_t) h_lo;
				high[i] = (uint32_t) ceil(h_lo);
				h_lo = log2((double)low[i]);
				h_hi = log2((double)high[i]);
				ratio[i] = (entropies[i] - h_hi) / (h_lo - h_hi);
				break;
			case BIT_ALLOC_MODE_INT_POWER:
				h_lo = floor(entropies[i]);
				h_hi = ceil(entropies[i]);
				low[i] = (uint32_t) pow(2, h_lo);
				high[i] = (uint32_t) pow(2, h_hi);
				ratio[i] = (entropies[i] - h_hi) / (h_lo - h_hi);
				break;
			case BIT_ALLOC_MODE_NO_MIX:
			default:
				ratio[i] = 1;
				low[i] = (uint32_t) floor(pow(2, entropies[i]));
				high[i] = 0;
				break;
		}
	}
	
	free_pmf_list(uc_pmf_list);
}

/**
 * Does state calculation, producing hi, lo, and ratio, for the given entropy value
 * @param entropy The entropy to use for state calculation
 * @param high Output, number of hi states stored here
 * @param low Output, number of lo states stored here
 * @param ratio Output, ratio between hi and lo to use
 */
void find_states(double entropy, uint32_t *high, uint32_t *low, double *ratio) {
	double h_hi, h_lo;

	// H = rH_lo + (1-r)H_hi
	// H - H_hi = r(H_lo - H_hi)
	// r = (H - H_hi) / (H_lo - H_hi)
	h_lo = pow(2.0, entropy);
	*low = (uint32_t) h_lo;
	*high = (uint32_t) ceil(h_lo);
	h_lo = log2((double)*low);
	h_hi = log2((double)*high);
	*ratio = (entropy - h_hi) / (h_lo - h_hi);
}


void compute_qpmf_quan_list(struct quantizer_t *q_lo, struct quantizer_t *q_hi, struct pmf_list_t *q_x_pmf, double ratio, struct alphabet_t *q_output_union){
    
    symbol_t x;
    
    uint32_t q_symbol, idx;
    
    for (x = 0; x < q_lo->alphabet->size; x++) {
        
        for (idx = 0; idx < q_output_union->size; idx++) {
        
            q_symbol = q_output_union->symbols[idx];
            
            if (q_lo->q[x] == q_symbol)
                q_x_pmf->pmfs[x]->pmf[idx] += ratio;
            
            if (q_hi->q[x] == q_symbol)
                q_x_pmf->pmfs[x]->pmf[idx] += (1-ratio);
        }
        
    }
    
    
}

void compute_qpmf_list(struct pmf_list_t *qpmf_list, struct cond_pmf_list_t *in_pmfs, uint32_t column, struct pmf_list_t *prev_qpmf_list, struct alphabet_t * q_alphabet_union, struct alphabet_t * prev_q_alphabet_union, struct cond_quantizer_list_t *q_list){
    
    symbol_t x;
    
    double p_q_xq = 0;
    
    uint32_t q_symbol, idx, k, j, q_prev_symbol;
    
    struct quantizer_t *q_hi, *q_lo;
    
    // compute P(Q_i | X_i)
    for (k = 0; k < qpmf_list->size; k++)
    {
        // compute P(Q_i | X_i = k)
        for (idx = 0; idx < q_alphabet_union->size; idx++)
        {
            q_symbol = q_alphabet_union->symbols[idx];
            
            // compute P(Q_i = q_symbol | X_i = k)
            
            j = 0;
            p_q_xq = 0.0;
            for (j = 0; j < prev_q_alphabet_union->size; j++) {
                
                // extract the jth quantizers of X_i;
                q_lo = get_cond_quantizer_indexed(q_list, column-1, 2*j);
                q_hi = get_cond_quantizer_indexed(q_list, column-1, (2*j)+1);
                
                q_prev_symbol = prev_q_alphabet_union->symbols[j];
                
                // Given the quantizers q_lo and q_hi, compute P(Q_i = q_symbol|X_i = k ,Q_{i-1} chooses the jth quantizer of X_i)
                
                if (q_lo->q[k] == q_symbol)
                    p_q_xq += q_lo->ratio;
                
                if (q_hi->q[k] == q_symbol)
                    p_q_xq += q_hi->ratio;
                
            
                for (x = 0; x < prev_qpmf_list->size; ++x) {
                
                    qpmf_list->pmfs[k]->pmf[idx] += get_probability(prev_qpmf_list->pmfs[x], j) * get_probability(get_cond_pmf(in_pmfs, column-1, x), k) * get_probability(in_pmfs->marginal_pmfs->pmfs[column-2], x);
                }
                
                qpmf_list->pmfs[k]->pmf[idx] *= p_q_xq;
            }
        }
        
        // Normilize P(Q_i | X_i = k)
        qpmf_list->pmfs[k]->pmf_ready = 1;
        renormalize_pmf(qpmf_list->pmfs[k]);
    }
    
}

void compute_xpmf_list(struct pmf_list_t *qpmf_list, struct cond_pmf_list_t *in_pmfs, uint32_t column, struct pmf_list_t *xpmf_list, struct alphabet_t * q_alphabet_union){
    
    symbol_t x;
    
    uint32_t q_symbol, idx, k;
    
    // compute P(X_{i+1} | Q_i)
    for (idx = 0; idx < q_alphabet_union->size; idx++)
    {
        // compute P(X_{i+1} | Q_i = q)
        q_symbol = q_alphabet_union->symbols[idx];
        
        for (k = 0; k < qpmf_list->size; k++)
        {
            // compute P(X_{i+1} = k | Q_i = q)
            
            for (x = 0; x < qpmf_list->size; ++x) {
                    
                xpmf_list->pmfs[idx]->pmf[k] += get_probability(qpmf_list->pmfs[x], idx) * get_probability(get_cond_pmf(in_pmfs, column, x), k) * get_probability(in_pmfs->marginal_pmfs->pmfs[column-1], x);
            }
        }
        // Normilize P(X_{i+1} | Q_i = q)
        xpmf_list->pmfs[idx]->pmf_ready = 1;
        renormalize_pmf(xpmf_list->pmfs[idx]);
    }
    
}

/**
 * Given the statistics calculated before, we need to compute the entire codebook's worth of
 * quantizers, as well as all of the PMFs and related stats
 */
struct cond_quantizer_list_t *generate_codebooks(struct quality_file_t *info, struct cond_pmf_list_t *in_pmfs, struct distortion_t *dist, double comp, uint32_t mode, double *expected_distortion) {
    
	// Stuff for state allocation and mixing
	uint32_t hi, lo;
	double ratio;
    
	// Miscellaneous variables
	uint32_t column, j;
	symbol_t q_symbol;
	double mse;
    
	// Output list of conditional quantizers
	struct cond_quantizer_list_t *q_list = alloc_conditional_quantizer_list(info->columns);
    
	// Constant alphabet of all possible input symbols
	const struct alphabet_t *A = in_pmfs->alphabet;
    
	// Temporary/extra pointers
	struct quantizer_t *q_lo;
    struct quantizer_t *q_hi;
    
	// List of conditionally quantized PMFs after quantizer has been added out
	struct pmf_list_t *xpmf_list;
    
	// List of conditionally quantized PMFs after the next quantizer was applied
	struct pmf_list_t *qpmf_list;
	struct pmf_list_t *prev_qpmf_list;
    
    
	// Alphabet of all possible quantizer outputs from the previous column
	struct alphabet_t *q_output_union;
    struct alphabet_t *q_prev_output_union;
    
    
    // Compute some statistics over the unquantized input
    calculate_statistics(info, in_pmfs);
    
    // For the column 0 the quantizers aren't conditional, so find them directly
    
    q_output_union = alloc_alphabet(1);
    cond_quantizer_init_column(q_list, 0, q_output_union);
    
    // Initialize the new pmfs (dummy)
    qpmf_list = alloc_pmf_list(A->size, q_output_union);
    xpmf_list = alloc_pmf_list(q_output_union->size, A);
    
    // Compute number of states for hi and lo, and ratio for the quantizers
    find_states(get_entropy(get_cond_pmf(in_pmfs, 0, 0)) * comp, &hi, &lo, &ratio);
	q_lo = generate_quantizer(get_cond_pmf(in_pmfs, 0, 0), dist, lo, &mse, ratio);
    q_hi = generate_quantizer(get_cond_pmf(in_pmfs, 0, 0), dist, hi, &mse, 1-ratio);
	
    store_cond_quantizers(q_lo, q_hi, q_list, 0, 0);
    
    // free the used pmfs and alphabet
    // (do not free q_prev_output_union and prev_qpmf_output as it's the first assignment).
    q_prev_output_union = q_output_union;
    
    prev_qpmf_list = qpmf_list;
    
    free_pmf_list(xpmf_list);
    
    // Start computing the quantizers of the rest of the columns
    
    for (column = 1; column < info->columns; column++) {
        
        // Compute the next output alphabet union over all quantizers for this column
		q_output_union = duplicate_alphabet(get_cond_quantizer_indexed(q_list, column-1, 0)->output_alphabet);
		for (j = 1; j < 2*q_prev_output_union->size; ++j) {
			alphabet_union(q_output_union, get_cond_quantizer_indexed(q_list, column-1, j)->output_alphabet, q_output_union);
		}
        cond_quantizer_init_column(q_list, column, q_output_union);
        
        // Initialize the new pmfs
        qpmf_list = alloc_pmf_list(A->size, q_output_union);
        xpmf_list = alloc_pmf_list(q_output_union->size, A);
        
        // Compute P(Q_i|X_i)
        if (column == 1)
            compute_qpmf_quan_list(q_lo, q_hi, qpmf_list, ratio, q_output_union);
        else
            compute_qpmf_list(qpmf_list, in_pmfs, column, prev_qpmf_list, q_output_union, q_prev_output_union, q_list);
        
        // Compute P(X_{i+1}|Q_i)
        compute_xpmf_list(qpmf_list, in_pmfs, column, xpmf_list, q_output_union);
        
        // for each previous value Q_i compute the quantizers
        for (j = 0; j < q_output_union->size; ++j) {
            
            q_symbol = q_output_union->symbols[j];
            
            // Find and save quantizers
            find_states(get_entropy(xpmf_list->pmfs[j]) * comp, &hi, &lo, &ratio);
            q_lo = generate_quantizer(xpmf_list->pmfs[j], dist, lo, &mse, ratio);
            q_hi = generate_quantizer(xpmf_list->pmfs[j], dist, hi, &mse, 1 - ratio);
            store_cond_quantizers(q_lo, q_hi, q_list, column, q_symbol);
        }
        
        // deallocated the memory of the used pmfs and alphabet
        free(q_prev_output_union);
        q_prev_output_union = q_output_union;
        
        free_pmf_list(prev_qpmf_list);
		prev_qpmf_list = qpmf_list;
        
        free_pmf_list(xpmf_list);

    }
    
	// Final cleanup, things we saved at the end of the final iteration that aren't needed
	free_pmf_list(qpmf_list);
    free(q_output_union);
    
	// TODO: Generate a codebook-format organization of the quantizers for quick lookup during encoding
	return q_list;
}

/**
 * Given the statistics calculated before, we need to compute the entire codebook's worth of
 * quantizers, as well as all of the PMFs and related stats
 * TODO: Add hi states to generation
 */
/*struct cond_quantizer_list_t *generate_codebooks(struct quality_file_t *info, struct cond_pmf_list_t *in_pmfs, struct distortion_t *dist, double comp, uint32_t mode, double *expected_distortion) {
	// Stuff for state allocation and mixing
	uint32_t *hi_states, *lo_states;
	uint32_t hi, lo;
	double ratio, Prob;

	// Miscellaneous variables
	uint32_t column, i, j, k;
	symbol_t q, x;
	double qnorm, norm, mse, column_mse, total_mse;

	// Output list of conditional quantizers
	struct cond_quantizer_list_t *q_list = alloc_conditional_quantizer_list(info->columns);

	// Constant alphabet of all possible input symbols
	const struct alphabet_t *A = in_pmfs->alphabet;

	// Temporary/extra pointers
	struct quantizer_t *q_lo;
    struct quantizer_t *q_hi;
	struct pmf_t *pmf_temp;
    
    // List of conditionally quantized PMFs for a given quantizer Q_{i-1}
	struct pmf_list_t *qpmf_quan_list;

	// List of conditionally quantized PMFs after quantizer has been added out
	struct pmf_list_t *xpmf_list;

	// List of conditionally quantized PMFs after the next quantizer was applied
	struct pmf_list_t *qpmf_list;
	struct pmf_list_t *prev_qpmf_list;

	// "Alphabet" of output quantizers for the previous column and PMF over that set
	struct alphabet_t *q_alphabet;
	struct pmf_t *q_pmf;
	struct alphabet_t *next_q_alphabet;
	struct pmf_t *next_q_pmf;

	// Alphabet of all possible quantizer outputs from the previous column
	struct alphabet_t *q_output_union;
	struct alphabet_t *next_output_union;

	// First we need to know what our training stats are and figure out how many states
	// to put into each quantizer
	calculate_statistics(info, in_pmfs);
	find_bit_allocation(in_pmfs, comp, &hi_states, &lo_states, &q_list->ratio, mode);

	// Set up a quantizer alphabet for column zero. The cond quantizer list duplicates the
	// alphabet internally so we don't need to worry about duplicating it
	q_alphabet = alloc_alphabet(1);
	cond_quantizer_init_column(q_list, 0, q_alphabet);
    
    
//	printf("MSE for column 0: %f\n", mse);

	// Initialize a 100% PMF for this quantizer alphabet
	q_pmf = alloc_pmf(q_alphabet);
	pmf_increment(q_pmf, 0);
	q_output_union = duplicate_alphabet(q_temp->output_alphabet);

	// Previous qpmf list needs to be initialized for a single quantizer's output
	prev_qpmf_list = alloc_pmf_list(1, A);
	apply_quantizer(q_temp, get_cond_pmf(in_pmfs, 0, 0), prev_qpmf_list->pmfs[0]);

	// Iterate over remaining columns and compute the quantizers and quantized PMFs
	for (column = 1; column < info->columns; ++column) {
		// Different approach to calculating xpmf, we should be able to find P(X_c = x | Q_c-1=q) for each
		// q in the possible list of quantizer outputs for the previous column
		xpmf_list = alloc_pmf_list(A->size, A);
//		pmf_temp = alloc_pmf(A);
		for (j = 0; j < q_output_union->size; ++j) {
			q = q_output_union->symbols[j];

			// Find normalizing constant for all quantizers that produce this Q symbol
			qnorm = 0;
			for (i = 0; i < q_alphabet->size; ++i) {
				q_temp = get_cond_quantizer_indexed(q_list, column-1, i);
				if (alphabet_contains(q_temp->output_alphabet, q)) {
					qnorm += get_probability(q_pmf, i);
				}
			}

			// Find appropriate combination of conditional probabilities weighted by quanizer
			for (i = 0; i < q_alphabet->size; ++i) {
				q_temp = get_cond_quantizer_indexed(q_list, column-1, i);
				for (x = 0; x < A->size; ++x) {
					norm = 0;
					for (k = 0; k < A->size; ++k) {
						if (q_temp->q[k] == q) {
							norm += get_probability(in_pmfs->marginal_pmfs->pmfs[column-1], k);
						}
					}
					for (k = 0; k < A->size; ++k) {
						if (q_temp->q[k] == q && norm > 0) {
							xpmf_list->pmfs[q]->pmf[x] += get_probability(get_cond_pmf(in_pmfs, column, k), x) * get_probability(in_pmfs->marginal_pmfs->pmfs[column-1], k) * get_probability(q_pmf, i) / (norm * qnorm);
						}
					}
				}
			}
			xpmf_list->pmfs[q]->pmf_ready = 1;
		}
		
			
		// So, first, compute the next set of quantizers and associated output PMFs
		cond_quantizer_init_column(q_list, column, q_output_union);
		qpmf_list = alloc_pmf_list(q_output_union->size, A);
		printf("MSE for column %d\n", column);
		column_mse = 0;
		for (j = 0; j < q_output_union->size; ++j) {
			q = q_output_union->symbols[j];

			// Find and save quantizer
			find_states(get_entropy(xpmf_list->pmfs[q]) * comp, &hi, &lo, &ratio);
			q_temp = generate_quantizer(xpmf_list->pmfs[q], dist, lo_states[column], &mse);
			store_cond_quantizers(q_temp, NULL, q_list, column, q);

			// Find the PMF of the quantizer's output
			apply_quantizer(q_temp, xpmf_list->pmfs[q], qpmf_list->pmfs[j]);

			// Calculate MSE contribution from this quantizer
			norm = 0.0;
			for (i = 0; i < q_alphabet->size; ++i) {
				q_temp = get_cond_quantizer_indexed(q_list, column-1, i);
				if (alphabet_contains(q_temp->output_alphabet, q)) {
					norm += get_probability(q_pmf, i) * get_probability(prev_qpmf_list->pmfs[i], q);
				}
			}
//			printf("MSE: %f, Pr: %f\t", mse, norm);
			total_mse += mse * norm;
			column_mse += mse * norm;
		}
		printf("Column MSE: %f.\n", column_mse);

		// Compute the next output alphabet union over all quantizers for this column
		next_output_union = duplicate_alphabet(get_cond_quantizer_indexed(q_list, column, 0)->output_alphabet);
		for (j = 1; j < q_output_union->size; ++j) {
			alphabet_union(next_output_union, get_cond_quantizer_indexed(q_list, column, j)->output_alphabet, next_output_union);
		}
//		printf("Output alphabet union:\n");
//		print_alphabet(next_output_union);

		// Find the pmf of choice of quantizer for this column
		next_q_alphabet = alloc_alphabet(q_output_union->size);
		next_q_pmf = alloc_pmf(next_q_alphabet);
//		printf("Number of quantizers: %d\n", next_q_alphabet->size);
		for (j = 0; j < next_q_alphabet->size; ++j) {
			// next_q_alphabet is the same size as q_output_union, but the first indexes quantizers
			// and the second is the unique set of quantizer outputs
			q = q_output_union->symbols[j];
//			printf("q: %d\n", q);
			for (i = 0; i < q_alphabet->size; ++i) {
				// Total probability over PMF of each symbol conditioned on the quantizer that produced it
				// Output symbol selects which quantizer is used from this column
//				printf("pmf[%d] = %f + (%f * %f)\n", j, next_q_pmf->pmf[j], get_probability(q_pmf, i), get_probability(prev_qpmf_list->pmfs[i], q));
//				print_pmf(prev_qpmf_list->pmfs[i]);
				next_q_pmf->pmf[j] += get_probability(q_pmf, i) * get_probability(prev_qpmf_list->pmfs[i], q);
			}
		}
		next_q_pmf->pmf_ready = 1;
//		print_pmf(next_q_pmf);

		// Finally, deallocate memory that was used on this iteration
		free(q_output_union);
		q_output_union = next_output_union;

		free_pmf_list(prev_qpmf_list);
		prev_qpmf_list = qpmf_list;
		free_pmf_list(xpmf_list);
//		for (i = 0; i < q_alphabet->size; ++i) {
//			free_pmf_list(cq_pmf_list[i]);
//		}
//		free(cq_pmf_list);

		free_pmf(q_pmf);
		q_pmf = next_q_pmf;
		free_alphabet(q_alphabet);
		q_alphabet = next_q_alphabet;
	}

	// Final cleanup, things we saved at the end of the final iteration that aren't needed
	free(q_pmf);
	free_pmf_list(qpmf_list);
	free(q_alphabet);

	if (expected_distortion != NULL)
		*expected_distortion = total_mse / info->columns;

	// TODO: Generate a codebook-format organization of the quantizers for quick lookup during encoding
	return q_list;
}
*/
// Legacy stuff for the current version; this should be updated as things are implemented
// using the new C-based calculations

/**
 * Writes a codebook to a file in the same format that we read it below, from the quantizer list
 */
void write_codebook(const char *filename, struct cond_quantizer_list_t *quantizers) {
	FILE *fp;
	uint32_t i, j, k, z;
	uint32_t columns = quantizers->columns;
	struct quantizer_t *q_temp = get_cond_quantizer_indexed(quantizers, 0, 0);
	uint32_t size = q_temp->alphabet->size;
	uint32_t buflen = columns > size ? columns : size;
	char *eol = "\n";
	char *linebuf = (char *) _alloca(sizeof(char)*buflen);
	char *empty = (char *) _alloca(sizeof(char)*buflen);

	fp = fopen(filename, "wt");
	if (!fp) {
		perror("Unable to open codebook file");
		exit(1);
	}

	// ASCII spaces are used to denote "unused" stuff
	memset(empty, 32, sizeof(char)*buflen);

	// First two lines are not used (number of states per column) but need to have the same length
	// as the number of columns
	fwrite(empty, sizeof(char), columns, fp);
	fwrite(eol, sizeof(char), 1, fp);
	fwrite(empty, sizeof(char), columns, fp);
	fwrite(eol, sizeof(char), 1, fp);

	// Next line is the ratio between states
	for (i = 0; i < columns; ++i) {
		linebuf[i] = ((uint8_t)(quantizers->ratio[i][0]*100)) + 33;
	}
	fwrite(linebuf, sizeof(char), columns, fp);
	fwrite(eol, sizeof(char), 1, fp);
	
	// Now, as we only have low states we will write each quantizer twice to match the format

	// Column 0 is handled specially
	for (i = 0; i < size; ++i) {
		linebuf[i] = q_temp->q[i] + 33;
	}
	fwrite(linebuf, sizeof(char), size, fp);
	fwrite(eol, sizeof(char), 1, fp);
	fwrite(linebuf, sizeof(char), size, fp);
	fwrite(eol, sizeof(char), 1, fp);

	for (i = 1; i < columns; ++i) {
		for (z = 0; z < 2; ++z) {
			for (j = 0; j < size; ++j) {
				q_temp = get_cond_quantizer(quantizers, i, j);
				if (q_temp) {
					for (k = 0; k < size; ++k) {
						linebuf[k] = q_temp->q[k] + 33;
					}
					fwrite(linebuf, sizeof(char), size, fp);
				}
				else {
					fwrite(empty, sizeof(char), size, fp);
				}
			}
			fwrite(eol, sizeof(char), 1, fp);
		}
	}

	fclose(fp);
}

/**
 * Reads in the codebook in the specified filename, calculates how many columns it is configured
 * for, and prepares the codebook list structure necessary to use for encoding with it
 */
uint32_t read_codebook(const char *filename, struct codebook_list_t *cb_list, uint8_t symbols) {
	FILE *fp;
	uint32_t columns;
	uint32_t column, j;
	char line[MAX_CODEBOOK_LINE_LENGTH];

	fp = fopen(filename, "rt");
	if (!fp) {
		perror("Unable to open codebook file");
		exit(1);
	}

	// Figure out how many columns the data has, accounting for the newline at the end of the line
	fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
	columns = strlen(line) - 1;

	// Initialize codebook based on knowing how many lines we have
	init_codebook_list(cb_list, symbols, columns);

	// Skip the first line (already read) and second line because we don't actually care about how many states there are
	fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);

	// Next line is the selection ratio between the two codebooks
	fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
	for (j = 0; j < columns; ++j) {
		cb_list->ratio[j] = line[j] - 33;
	}

	// Now, the lines in file alternate definitions for each codebook (low then high), so copy them over and generate the corresponding unique lists
	fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
	memcpy(cb_list->low[0][0].quantizer, line, symbols * sizeof(uint8_t));
	generate_uniques(&cb_list->low[0][0]);

	fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
	memcpy(cb_list->high[0][0].quantizer, line, symbols * sizeof(uint8_t));
	generate_uniques(&cb_list->high[0][0]);

	// Parse remaining lines as code books conditional on previous column values
	for (column = 1; column < columns; ++column) {
		fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
		for (j = 0; j < symbols; ++j) {
			memcpy(cb_list->low[column][j].quantizer, &line[j*symbols], symbols * sizeof(uint8_t));
			generate_uniques(&cb_list->low[column][j]);
		}

		fgets(line, MAX_CODEBOOK_LINE_LENGTH, fp);
		for (j = 0; j < symbols; ++j) {
			memcpy(cb_list->high[column][j].quantizer, &line[j*symbols], symbols * sizeof(uint8_t));
			generate_uniques(&cb_list->high[column][j]);
		}
	}

	// Done reading code book
	fclose(fp);
	return columns;
}

/**
 * Initializes a codebook list with storage space for an adequate number of codebooks
 * all initialized to all zeros, for the given number of possible symbols
 */
void init_codebook_list(struct codebook_list_t *list, uint8_t symbols, uint32_t columns) {
	
	// First, allocate arrays based on number of columns within the codebook list
	list->high = (struct codebook_t **) calloc(columns, sizeof(struct codebook_t *));
	list->low = (struct codebook_t **) calloc(columns, sizeof(struct codebook_t *));
	list->ratio = (uint8_t *) calloc(columns, sizeof(uint8_t));
	list->select_count = (uint32_t *) calloc(columns, sizeof(uint32_t));
	list->symbols = symbols;
	list->columns = columns;

	// Now allocate for each array
	init_codebook_array(list->high, symbols, columns);
	init_codebook_array(list->low, symbols, columns);

	// Make sure that WELL starts off reasonable
	list->well.n = 0;
}

/**
 * Initializes an array of codebooks, used when initializing a codebook list
 */
void init_codebook_array(struct codebook_t **cb, uint8_t symbols, uint32_t columns) {
	uint32_t c, s;

	// First column is special in that it only has one codebook, because there is no left context
	cb[0] = (struct codebook_t *) calloc(1, sizeof(struct codebook_t));
	cb[0]->quantizer = (uint8_t *) calloc(symbols, sizeof(uint8_t));
	cb[0]->uniques = (uint8_t *) calloc(symbols, sizeof(uint8_t));
	cb[0]->symbols = symbols;

	for (c = 1; c < columns; ++c) {
		cb[c] = (struct codebook_t *) calloc(symbols, sizeof(struct codebook_t));
		for (s = 0; s < symbols; ++s) {
			cb[c][s].symbols = symbols;
			cb[c][s].quantizer = (uint8_t *) calloc(symbols, sizeof(uint8_t));
			cb[c][s].uniques = (uint8_t *) calloc(symbols, sizeof(uint8_t));
		}
	}
}

/**
 * Walks over the quantizer string to determine how many unique symbols are present
 */
void generate_uniques(struct codebook_t *cb) {
	uint8_t u = 0;
	uint8_t s;

	cb->uniques[0] = cb->quantizer[0];
	for (s = 1; s < cb->symbols; ++s) {
		if (cb->quantizer[s] != cb->uniques[u]) {
			u += 1;
			cb->uniques[u] = cb->quantizer[s];
		}
	}

	cb->actual_unique_count = u+1;
	cb->bits = cb_log2(cb->actual_unique_count);
}

/**
 * Selects a codebook for the given column from the codebook list with the appropriate ratio
 */
struct codebook_t *choose_codebook(struct codebook_list_t *list, uint32_t column, uint8_t prev_value) {
	if (well_1024a(&list->well) % 100 >= list->ratio[column]) {
		list->select_count[column] += 1;
		return &list->high[column][prev_value];
	}
	return &list->low[column][prev_value];
}

/**
 * Converts the quality score into a state number that can be stored in fewer bits
 * @param value is an ascii character to be converted into a numbered state
 * @deprecated old codebooks, remove
uint8_t find_state_encoding(struct codebook_t *codebook, uint8_t value) {
	uint8_t u;

	for (u = 0; u < codebook->actual_unique_count; ++u) {
		if (codebook->uniques[u] == value)
			return u;
	}
	return u;
}
*/

/**
 * Displays a codebook on STDOUT
 * @deprecated, remove as the quantizer print methods handle this now
 */
void print_codebook(struct codebook_t *cb) {
	uint8_t s = 0;
	uint8_t *tmp;

	tmp = (uint8_t *) calloc(cb->symbols+1, sizeof(uint8_t));

	for (s = 0; s < cb->symbols; ++s) {
		memcpy(tmp, cb[s].quantizer, cb->symbols);
		printf("%d (%c):\t%s\n", s, (uint8_t) (s+33), tmp);
	}

	free(tmp);
}

