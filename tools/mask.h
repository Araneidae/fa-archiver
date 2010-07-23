/* Filter mask routines. */

/* Bit mask of BPM ids, array of 256 bits. */
typedef uint32_t filter_mask_t[FA_ENTRY_COUNT/32];


static inline void set_mask_bit(filter_mask_t mask, int bit)
{
    mask[bit >> 5] |= 1 << (bit & 0x1f);
}

static inline bool test_mask_bit(filter_mask_t mask, int bit)
{
    return !!(mask[bit >> 5] & (1 << (bit & 0x1f)));
}

/* Returns number of bits set in mask. */
int count_mask_bits(filter_mask_t mask);


#define RAW_MASK_BYTES  (FA_ENTRY_COUNT / 4)

/* Formats string represetation of mask into buffer, which must be at least
 * RAW_MASK_BYTES+1 bytes long.  Returns RAW_MASK_BYTES, number of characters
 * written. */
int format_raw_mask(filter_mask_t mask, char *buffer);
/* Prints mask. */
void print_raw_mask(FILE *out, filter_mask_t mask);

/* Attempts to parse string as a mask specification, consisting of a sequence
 * of comma separated numbers or ranges, where a range is a pair of numbers
 * separated by -.  In other words:
 *
 *  mask = number [ "," mask ] | range [ "," mask ]
 *  range = number "-" number
 *
 * Prints error message and returns false if parsing fails. */
bool parse_mask(const char *string, filter_mask_t mask);

/* Parses a mask in the form generated by format_mask(), namely a sequence of 64
 * hex digits. */
bool parse_raw_mask(const char *string, filter_mask_t mask);

/* Copies a single FA frame taking the mask into account, returns the number
 * of bytes copied into the target buffer (will be 8*count_mask_bits(mask)).
 * 'from' should point to a completely populated frame, 'to' will contain X,Y
 * pairs in ascending numerical order for bits set in mask. */
int copy_frame(void *to, void *from, filter_mask_t mask);

/* Writes the selected number of masked frames to the given file, returning
 * false if writing fails. */
bool write_frames(int file, filter_mask_t mask, void *frame, int count);
