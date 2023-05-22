#include <stdlib.h>
#include <stdio.h>

#include "alphabet.h"
#include "klv.h"
#include "rack.h"

int count_words_at(KLV * klv, int p, int kwg_size) {
    if (p >= kwg_size) {
        return 0;
    }
    if (klv->word_counts[p] == -1) {
        printf("unexpected -1 at %d\n", p);
        abort();
    }
    if (klv->word_counts[p] == 0) {
        klv->word_counts[p] = -1;

        int a = 0;
        if (kwg_accepts(klv->kwg, p)) {
            a = 1;
        }
        int b = 0;
        int arc_index_p = kwg_arc_index(klv->kwg, p);
        if (arc_index_p != 0) {
            b = count_words_at(klv, arc_index_p, kwg_size);
        }
        int c = 0;
        int is_not_end = !kwg_is_end(klv->kwg, p);
        if (is_not_end) {
            c = count_words_at(klv, p + 1, kwg_size);
        }
        klv->word_counts[p] = a + b + c;
    }
    return klv->word_counts[p];
}

void count_words(KLV * klv, size_t kwg_size) {
    for (int p = kwg_size - 1; p >= 0; p--) {
        count_words_at(klv, p, (int)kwg_size);
    }
}

void print_float_bits(float value) {
    unsigned int* float_bits = (unsigned int*)&value;

    // Obtain the bit representation
    unsigned int bits = *float_bits;

    // Print the bits
    for (int i = sizeof(float) * 8 - 1; i >= 0; i--) {
        unsigned int bit = (bits >> i) & 1;
        printf("%u", bit);
    }
    printf("\n");
}

// Egregious hack to convert endianness of a float
float convert_little_endian_to_host(float little_endian_float) {
    uint32_t* little_endian_bits = (uint32_t*)&little_endian_float;
    uint32_t host_bits = *little_endian_bits;

    // Check if host machine is little-endian
    union {
        uint32_t i;
        char c[4];
    } endian_check = {0x01020304};

    if (endian_check.c[0] == 4) {
        // Host machine is little-endian
        return *((float*)&host_bits);
    } else {
        // Host machine is big-endian, swap bytes
        uint32_t big_endian_bits =
            ((host_bits & 0xFF000000) >> 24) |
            ((host_bits & 0x00FF0000) >> 8) |
            ((host_bits & 0x0000FF00) << 8) |
            ((host_bits & 0x000000FF) << 24);

        return *((float*)&big_endian_bits);
    }
}

void load_klv(KLV * klv, const char* klv_filename) {
	FILE * stream;
	stream = fopen(klv_filename, "r");
	if (stream == NULL) {
		perror(klv_filename);
		exit(EXIT_FAILURE);
	}

    uint32_t kwg_size;
    size_t result;

	result = fread(&kwg_size, sizeof(kwg_size), 1, stream);
	if (result != 1) {
		printf("kwg size fread failure: %zd != %d\n", result, 1);
		exit(EXIT_FAILURE);
	}
	kwg_size = le32toh(kwg_size);

    klv->kwg = malloc(sizeof(KWG));
    klv->kwg->nodes = (uint32_t *) malloc(kwg_size*sizeof(uint32_t));
	result = fread(klv->kwg->nodes, sizeof(uint32_t), kwg_size, stream);
	if (result != kwg_size) {
		printf("kwg nodes fread failure: %zd != %d\n", result, kwg_size);
		exit(EXIT_FAILURE);
	}
	for (uint32_t i = 0; i < kwg_size; i++) {
		klv->kwg->nodes[i] = le32toh(klv->kwg->nodes[i]);
	}

    uint32_t number_of_leaves;
	result = fread(&number_of_leaves, sizeof(number_of_leaves), 1, stream);
	if (result != 1) {
		printf("number of leaves fread failure: %zd != %d\n", result, 1);
		exit(EXIT_FAILURE);
	}
	number_of_leaves = le32toh(number_of_leaves);

	klv->leave_values = (float *) malloc(number_of_leaves*sizeof(float));
	result = fread(klv->leave_values, sizeof(float), number_of_leaves, stream);
	if (result != number_of_leaves) {
		printf("edges fread failure: %zd != %d\n", result, number_of_leaves);
		exit(EXIT_FAILURE);
	}

    fclose(stream);

	for (uint32_t i = 0; i < number_of_leaves; i++) {
		klv->leave_values[i] = convert_little_endian_to_host(klv->leave_values[i]);
	}

    klv->word_counts =  (int *) malloc(kwg_size*sizeof(int));
    for (size_t i = 0; i < kwg_size; i++) {
        klv->word_counts[i] = 0;
    }

    count_words(klv, kwg_size);

    // The KLV does not use the alphabet in the KWG
    klv->kwg->alphabet = NULL;
}

KLV * create_klv(const char* klv_filename) {
    KLV * klv =  malloc(sizeof(KLV));
    load_klv(klv, klv_filename);
    return klv;
}

void destroy_klv(KLV * klv) {
    destroy_kwg(klv->kwg);
    free(klv->leave_values);
    free(klv->word_counts);
    free(klv);
}

int get_word_index_of(KLV * klv, uint32_t node_index, Rack * leave) {
    int idx = 0;
    int lidx = 0;
    int lidx_letter_count = leave->array[lidx];
    int number_of_letters = leave->number_of_letters;

    // Advance lidx
    while (lidx_letter_count == 0) {
        lidx++;
        lidx_letter_count = leave->array[lidx];
    }

    while (node_index != 0) {
        while (kwg_tile(klv->kwg, node_index) != (uint8_t)lidx) {
            if (kwg_is_end(klv->kwg, node_index)) {
                return -1;
            }
            idx += klv->word_counts[node_index] - klv->word_counts[node_index+1];
            node_index++;
        }

        lidx_letter_count--;
        number_of_letters--;

        // Advance lidx
        while (lidx_letter_count == 0) {
            lidx++;
            if (lidx >= leave->array_size) {
                break;
            }
            lidx_letter_count = leave->array[lidx];
        }

        if (number_of_letters == 0) {
            if (kwg_accepts(klv->kwg, node_index)) {
                return idx;
            }
            return -1;
        }
        if (kwg_accepts(klv->kwg, node_index)) {
            idx += 1;
        }
        node_index = kwg_arc_index(klv->kwg, node_index);
    }
    return -1;
}


float leave_value(KLV * klv, Rack * leave) {
    if (leave->empty) {
        return 0.0;
    }
    int index = get_word_index_of(klv, kwg_arc_index(klv->kwg, 0), leave);
    if (index != -1) {
        return klv->leave_values[index];
    }
    return 0.0;
}