/**
 * aligner_sw_sse.cpp
 *
 * Versions of key alignment functions that use vector instructions to
 * accelerate dynamic programming.  Based chiefly on the striped Smith-Waterman
 * paper and implementation by Michael Farrar.  See:
 *
 * Farrar M. Striped Smith-Waterman speeds database searches six times over
 * other SIMD implementations. Bioinformatics. 2007 Jan 15;23(2):156-61.
 * http://sites.google.com/site/farrarmichael/smith-waterman
 *
 * While the paper describes an implementation of Smith-Waterman, we extend it
 * do end-to-end read alignment as well as local alignment.  The change
 * required for this is minor: we simply let vmax be the maximum element in the
 * score domain rather than the minimum.
 *
 * The vectorized dynamic programming implementation lacks some features that
 * make it hard to adapt to solving the entire dynamic-programming alignment
 * problem.  For instance:
 *
 * - It doesn't respect gap barriers on either end of the read
 * - It just gives a maximum; not enough information to backtrace without
 *   redoing some alignment
 * - It's a little difficult to handle st_ and en_, especially st_.
 * - The query profile mechanism makes handling of ambiguous reference bases a
 *   little tricky (16 cols in query profile lookup table instead of 5)
 *
 * Given the drawbacks, it is tempting to use SSE dynamic programming as a
 * filter rather than as an aligner per se.  Here are a few ideas for how it
 * can be extended to handle more of the alignment problem:
 *
 * - Save calculated scores to a big array as we go.  We return to this array
 *   to find and backtrace from good solutions.
 */

#ifndef NO_SSE

#include "aligner_sw.h"

static const size_t NBYTES_PER_REG  = 16;
static const size_t NWORDS_PER_REG  = 16;
static const size_t NBITS_PER_WORD  = 8;
static const size_t NBYTES_PER_WORD = 1;

// In local mode, we start low (0) and go high (255).  Factoring in a query
// profile involves unsigned saturating addition.  All query profile elements
// should be expressed as a positive number; this is done by adding -min
// where min is the smallest (negative) score in the query profile.

typedef uint8_t TCScore;

/**
 * Build query profile look up tables for the read.  The query profile look
 * up table is organized as a 1D array indexed by [i][j] where i is the
 * reference character in the current DP column (0=A, 1=C, etc), and j is
 * the segment of the query we're currently working on.
 */
void SwAligner::buildQueryProfileLocalSseU8(bool fw) {
	const BTDnaString* rd = fw ? rdfw_ : rdrc_;
	const BTString* qu = fw ? qufw_ : qurc_;
	const size_t len = rd->length();
	const size_t seglen = (len + (NWORDS_PER_REG-1)) / NWORDS_PER_REG;
	// How many __m128i's are needed
	size_t n128s =
		64 +                    // slack bytes, for alignment?
		(seglen * ALPHA_SIZE)   // query profile data
		* 2;                    // & gap barrier data
	assert_gt(n128s, 0);
	SSEData& d = fw ? sseU8fw_ : sseU8rc_;
	d.buf_.resize(n128s * sizeof(__m128i));
	// Get a 16-byte aligned pointer toward the beginning of the buffer.
	size_t aligned = ((size_t)d.buf_.ptr() + 15) & ~(0x0f);
	// Set up pointers into the buffer for fw query
	d.qprof_       = reinterpret_cast<__m128i*>(aligned);
	d.maxPen_      = d.maxBonus_ = 0;
	d.lastIter_    = d.lastWord_ = 0;
	d.qprofStride_ = d.gbarStride_ = 2;
	d.bias_ = 0;
	// Calculate bias
	for(size_t refc = 0; refc < ALPHA_SIZE; refc++) {
		for(size_t i = 0; i < len; i++) {
			int readc = (*rd)[i];
			int readq = (*qu)[i];
			int sc = sc_->score(readc, (int)(1 << refc), readq - 33);
			if(sc < 0 && sc < d.bias_) {
				d.bias_ = sc;
			}
		}
	}
	assert_leq(d.bias_, 0);
	d.bias_ = -d.bias_;
	// For each reference character A, C, G, T, N ...
	for(size_t refc = 0; refc < ALPHA_SIZE; refc++) {
		// For each segment ...
		for(size_t i = 0; i < seglen; i++) {
			size_t j = i;
			uint8_t *qprofWords =
				reinterpret_cast<uint8_t*>(d.qprof_ + (refc * seglen * 2) + (i * 2));
			uint8_t *gbarWords =
				reinterpret_cast<uint8_t*>(d.qprof_ + (refc * seglen * 2) + (i * 2) + 1);
			// For each sub-word (byte) ...
			for(size_t k = 0; k < NWORDS_PER_REG; k++) {
				int sc = 0;
				*gbarWords = 0;
				if(j < len) {
					int readc = (*rd)[j];
					int readq = (*qu)[j];
					sc = sc_->score(readc, (int)(1 << refc), readq - 33);
					assert_range(0, 255, sc + d.bias_);
					size_t j_from_end = len - j - 1;
					if(j < (size_t)sc_->gapbar ||
					   j_from_end < (size_t)sc_->gapbar)
					{
						// Inside the gap barrier
						*gbarWords = 0xff;
					}
				}
				if(refc == 0 && j == len-1) {
					// Remember which 128-bit word and which smaller word has
					// the final row
					d.lastIter_ = i;
					d.lastWord_ = k;
				}
				if(sc < 0) {
					if((size_t)(-sc) > d.maxPen_) {
						d.maxPen_ = (size_t)(-sc);
					}
				} else {
					if((size_t)sc > d.maxBonus_) {
						d.maxBonus_ = (size_t)sc;
					}
				}
				*qprofWords = (uint8_t)(sc + d.bias_);
				gbarWords++;
				qprofWords++;
				j += seglen; // update offset into query
			}
		}
	}
}

#ifdef NDEBUG

#define assert_all_eq0(x)
#define assert_all_gt(x, y)
#define assert_all_gt_lo(x)
#define assert_all_lt(x, y)
#define assert_all_lt_hi(x)

#else

#define assert_all_eq0(x) { \
	__m128i z, tmp; \
	z = _mm_xor_si128(z, z); \
	tmp = _mm_cmpeq_epi16(x, z); \
	assert_eq(0xffff, _mm_movemask_epi8(tmp)); \
}

#define assert_all_gt(x, y) { \
	__m128i tmp = _mm_cmpgt_epu8(x, y); \
	assert_eq(0xffff, _mm_movemask_epi8(tmp)); \
}

#define assert_all_gt_lo(x) { \
	__m128i z, tmp; \
	z = _mm_xor_si128(z, z); \
	tmp = _mm_cmpgt_epu8(x, z); \
	assert_eq(0xffff, _mm_movemask_epi8(tmp)); \
}

#define assert_all_lt(x, y) { \
	__m128i z; \
	z = _mm_xor_si128(z, z); \
	__m128i tmp = _mm_subs_epu8(y, x); \
	tmp = _mm_cmpeq_epi16(tmp, z); \
	assert_eq(0x0000, _mm_movemask_epi8(tmp)); \
}

#define assert_all_lt_hi(x) { \
	__m128i z, tmp; \
	z = _mm_cmpeq_epu8(z, z); \
	z = _mm_srli_epu8(z, 1); \
	tmp = _mm_cmplt_epu8(x, z); \
	assert_eq(0xffff, _mm_movemask_epi8(tmp)); \
}
#endif

/**
 * Solve the current alignment problem using SSE instructions that operate on 16
 * unsigned 8-bit values packed into a single 128-bit register.
 */
TAlScore SwAligner::alignNucleotidesLocalSseU8(int& flag) {
	assert_leq(rdf_, rd_->length());
	assert_leq(rdf_, qu_->length());
	assert_lt(rfi_, rff_);
	assert_lt(rdi_, rdf_);
	assert_eq(rd_->length(), qu_->length());
	assert_geq(sc_->gapbar, 1);
	assert(en_ == NULL || en_->size() == solwidth_);
	assert(repOk());
#ifndef NDEBUG
	for(size_t i = rfi_; i < rff_; i++) {
		assert_range(0, 16, (int)rf_[i]);
	}
#endif

	SSEData& d = fw_ ? sseU8fw_ : sseU8rc_;
	assert(!d.buf_.empty());
	assert(d.qprof_ != NULL);

	assert_gt(d.maxBonus_, 0);
	size_t iter =
		(dpRows() + (NWORDS_PER_REG-1)) / NWORDS_PER_REG; // iter = segLen

	int dup;
	
	// Many thanks to Michael Farrar for releasing his striped Smith-Waterman
	// implementation:
	//
	//  http://sites.google.com/site/farrarmichael/smith-waterman
	//
	// Much of the implmentation below is adapted from Michael's code.

	// Set all elts to reference gap open penalty
	__m128i rfgapo   = _mm_setzero_si128();
	__m128i rfgape   = _mm_setzero_si128();
	__m128i rdgapo   = _mm_setzero_si128();
	__m128i rdgape   = _mm_setzero_si128();
	__m128i vlo      = _mm_setzero_si128();
	__m128i vfs      = _mm_setzero_si128();
	__m128i vvetohi  = _mm_setzero_si128();
	__m128i vhi      = _mm_setzero_si128();
	__m128i vmax     = _mm_setzero_si128();
	__m128i ve       = _mm_setzero_si128();
	__m128i vf       = _mm_setzero_si128();
	__m128i vh       = _mm_setzero_si128();
	__m128i vtmp     = _mm_setzero_si128();
	__m128i vzero    = _mm_setzero_si128();
	__m128i vbias    = _mm_setzero_si128();

	assert_gt(sc_->refGapOpen(), 0);
	assert_leq(sc_->refGapOpen(), std::numeric_limits<TCScore>::max());
	dup = (sc_->refGapOpen() << 8) | (sc_->refGapOpen() & 0x00ff);
	rfgapo = _mm_insert_epi16(rfgapo, dup, 0);
	rfgapo = _mm_shufflelo_epi16(rfgapo, 0);
	rfgapo = _mm_shuffle_epi32(rfgapo, 0);
	
	// Set all elts to reference gap extension penalty
	assert_gt(sc_->refGapExtend(), 0);
	assert_leq(sc_->refGapExtend(), std::numeric_limits<TCScore>::max());
	assert_leq(sc_->refGapExtend(), sc_->refGapOpen());
	dup = (sc_->refGapExtend() << 8) | (sc_->refGapExtend() & 0x00ff);
	rfgape = _mm_insert_epi16(rfgape, dup, 0);
	rfgape = _mm_shufflelo_epi16(rfgape, 0);
	rfgape = _mm_shuffle_epi32(rfgape, 0);

	// Set all elts to read gap open penalty
	assert_gt(sc_->readGapOpen(), 0);
	assert_leq(sc_->readGapOpen(), std::numeric_limits<TCScore>::max());
	dup = (sc_->readGapOpen() << 8) | (sc_->readGapOpen() & 0x00ff);
	rdgapo = _mm_insert_epi16(rdgapo, dup, 0);
	rdgapo = _mm_shufflelo_epi16(rdgapo, 0);
	rdgapo = _mm_shuffle_epi32(rdgapo, 0);
	
	// Set all elts to read gap extension penalty
	assert_gt(sc_->readGapExtend(), 0);
	assert_leq(sc_->readGapExtend(), std::numeric_limits<TCScore>::max());
	assert_leq(sc_->readGapExtend(), sc_->readGapOpen());
	dup = (sc_->readGapExtend() << 8) | (sc_->readGapExtend() & 0x00ff);
	rdgape = _mm_insert_epi16(rdgape, dup, 0);
	rdgape = _mm_shufflelo_epi16(rdgape, 0);
	rdgape = _mm_shuffle_epi32(rdgape, 0);
	
	vhi = _mm_cmpeq_epi16(vhi, vhi); // all elts = 0xffff
	vlo = _mm_xor_si128(vlo, vlo);   // all elts = 0
	vmax = vlo;
	
	// Set all elts to 0xffff
	vfs = _mm_cmpeq_epi16(vfs, vfs);
	vvetohi = _mm_slli_si128(vfs, NBYTES_PER_WORD);
	
	// Make a vector of bias offsets
	dup = (d.bias_ << 8) | (d.bias_ & 0x00ff);
	vbias = _mm_insert_epi16(vbias, dup, 0);
	vbias = _mm_shufflelo_epi16(vbias, 0);
	vbias = _mm_shuffle_epi32(vbias, 0);
	
	// Points to a long vector of __m128i where each element is a block of
	// contiguous cells in the E, F or H matrix.  If the index % 3 == 0, then
	// the block of cells is from the E matrix.  If index % 3 == 1, they're
	// from the F matrix.  If index % 3 == 2, then they're from the H matrix.
	// Blocks of cells are organized in the same interleaved manner as they are
	// calculated by the Farrar algorithm.
	const __m128i *pvScore; // points into the query profile

	d.mat_.init(dpRows(), rff_ - rfi_, NWORDS_PER_REG);
	const size_t colstride = d.mat_.colstride();
	const size_t rowstride = d.mat_.rowstride();
	assert_eq(rowstride, colstride / iter);
	
	// Initialize the H and E vectors in the first matrix column
	__m128i *pvHTmp = d.mat_.tmpvec(0, 0);
	__m128i *pvETmp = d.mat_.evec(0, 0);
	
	for(size_t i = 0; i < iter; i++) {
		_mm_store_si128(pvETmp, vlo);
		_mm_store_si128(pvHTmp, vlo); // start low in local mode
		pvETmp += rowstride;
		pvHTmp += rowstride;
	}
	// These are swapped just before the innermost loop
	__m128i *pvHStore = d.mat_.hvec(0, 0);
	__m128i *pvHLoad  = d.mat_.tmpvec(0, 0);
	__m128i *pvELoad  = d.mat_.evec(0, 0);
	__m128i *pvEStore = d.mat_.evecUnsafe(0, 1);
	__m128i *pvFStore = d.mat_.fvec(0, 0);
	__m128i *pvFTmp   = NULL;
	
	assert_gt(sc_->gapbar, 0);
	
	// Fill in the table as usual but instead of using the same gap-penalty
	// vector for each iteration of the inner loop, load words out of a
	// pre-calculated gap vector parallel to the query profile.  The pre-
	// calculated gap vectors enforce the gap barrier constraint by making it
	// infinitely costly to introduce a gap in barrier rows.
	//
	// AND use a separate loop to fill in the first row of the table, enforcing
	// the st_ constraints in the process.  This is awkward because it
	// separates the processing of the first row from the others and might make
	// it difficult to use the first-row results in the next row, but it might
	// be the simplest and least disruptive way to deal with the st_ constraint.
	
	for(size_t i = rfi_; i < rff_; i++) {
		assert(pvFStore == d.mat_.fvec(0, i - rfi_));
		assert(pvHStore == d.mat_.hvec(0, i - rfi_));
		
		// Fetch the appropriate query profile.  Note that elements of rf_ must
		// be numbers, not masks.
		const int refc = (int)rf_[i];
		size_t off = (size_t)firsts5[refc] * iter * 2;
		pvScore = d.qprof_ + off; // even elts = query profile, odd = gap barrier
		
		// Set all cells to low value
		vf = _mm_xor_si128(vf, vf);

		// Load H vector from the final row of the previous column
		vh = _mm_load_si128(pvHLoad + colstride - rowstride);
		// Shift 2 bytes down so that topmost (least sig) cell gets 0
		vh = _mm_slli_si128(vh, NBYTES_PER_WORD);
		
		// We pull out one loop iteration to make it easier to veto values in the top row
		
		// Load cells from E, calculated previously
		ve = _mm_load_si128(pvELoad);
		assert_all_lt(ve, vhi);
		pvELoad += rowstride;
		
		// Store cells in F, calculated previously
		// No need to veto ref gap extensions, they're all 0x00s
		_mm_store_si128(pvFStore, vf);
		pvFStore += rowstride;
		
		// Factor in query profile (matches and mismatches)
		vh = _mm_adds_epu8(vh, pvScore[0]);
		vh = _mm_subs_epu8(vh, vbias);
		
		// Update H, factoring in E and F
		vh = _mm_max_epu8(vh, ve);
		vh = _mm_max_epu8(vh, vf);
		
		// Update highest score encountered this far
		vmax = _mm_max_epu8(vmax, vh);
		
		// Save the new vH values
		_mm_store_si128(pvHStore, vh);
		pvHStore += rowstride;
		
		// Update vE value
		vtmp = vh;
		vh = _mm_subs_epu8(vh, rdgapo);
		vh = _mm_subs_epu8(vh, pvScore[1]); // veto some read gap opens
		ve = _mm_subs_epu8(ve, rdgape);
		ve = _mm_max_epu8(ve, vh);
		assert_all_lt(ve, vhi);
		
		// Load the next h value
		vh = _mm_load_si128(pvHLoad);
		pvHLoad += rowstride;
		
		// Save E values
		_mm_store_si128(pvEStore, ve);
		pvEStore += rowstride;
		
		// Update vf value
		vtmp = _mm_subs_epu8(vtmp, rfgapo);
		vf = _mm_subs_epu8(vf, rfgape);
		assert_all_lt(vf, vhi);
		vf = _mm_max_epu8(vf, vtmp);
		
		pvScore += 2; // move on to next query profile

		// For each character in the reference text:
		size_t j;
		for(j = 1; j < iter; j++) {
			// Load cells from E, calculated previously
			ve = _mm_load_si128(pvELoad);
			assert_all_lt(ve, vhi);
			pvELoad += rowstride;
			
			// Store cells in F, calculated previously
			vf = _mm_subs_epu8(vf, pvScore[1]); // veto some ref gap extensions
			_mm_store_si128(pvFStore, vf);
			pvFStore += rowstride;
			
			// Factor in query profile (matches and mismatches)
			vh = _mm_adds_epu8(vh, pvScore[0]);
			vh = _mm_subs_epu8(vh, vbias);
			
			// Update H, factoring in E and F
			vh = _mm_max_epu8(vh, ve);
			vh = _mm_max_epu8(vh, vf);
			
			// Update highest score encountered this far
			vmax = _mm_max_epu8(vmax, vh);
			
			// Save the new vH values
			_mm_store_si128(pvHStore, vh);
			pvHStore += rowstride;
			
			// Update vE value
			vtmp = vh;
			vh = _mm_subs_epu8(vh, rdgapo);
			vh = _mm_subs_epu8(vh, pvScore[1]); // veto some read gap opens
			ve = _mm_subs_epu8(ve, rdgape);
			ve = _mm_max_epu8(ve, vh);
			assert_all_lt(ve, vhi);
			
			// Load the next h value
			vh = _mm_load_si128(pvHLoad);
			pvHLoad += rowstride;
			
			// Save E values
			_mm_store_si128(pvEStore, ve);
			pvEStore += rowstride;
			
			// Update vf value
			vtmp = _mm_subs_epu8(vtmp, rfgapo);
			vf = _mm_subs_epu8(vf, rfgape);
			assert_all_lt(vf, vhi);
			vf = _mm_max_epu8(vf, vtmp);
			
			pvScore += 2; // move on to next query profile / gap veto
		}
		// pvHStore, pvELoad, pvEStore have all rolled over to the next column
		pvFTmp = pvFStore;
		pvFStore -= colstride; // reset to start of column
		vtmp = _mm_load_si128(pvFStore);
		
		pvHStore -= colstride; // reset to start of column
		vh = _mm_load_si128(pvHStore);
		
		pvEStore -= colstride; // reset to start of column
		ve = _mm_load_si128(pvEStore);
		
		pvHLoad = pvHStore;    // new pvHLoad = pvHStore
		pvScore = d.qprof_ + off + 1; // reset veto vector
		
		// vf from last row gets shifted down by one to overlay the first row
		// rfgape has already been subtracted from it.
		vf = _mm_slli_si128(vf, NBYTES_PER_WORD);
		
		vf = _mm_subs_epu8(vf, *pvScore); // veto some ref gap extensions
		vf = _mm_max_epu8(vtmp, vf);
		vtmp = _mm_subs_epu8(vf, vtmp);
		vtmp = _mm_cmpeq_epi8(vtmp, vzero);
		int cmp = _mm_movemask_epi8(vtmp);
		
		// If any element of vtmp is greater than H - gap-open...
		j = 0;
		while(cmp != 0xffff) {
			// Store this vf
			_mm_store_si128(pvFStore, vf);
			pvFStore += rowstride;
			
			// Update vh w/r/t new vf
			vh = _mm_max_epu8(vh, vf);
			
			// Save vH values
			_mm_store_si128(pvHStore, vh);
			pvHStore += rowstride;
			
			// Update highest score encountered this far
			vmax = _mm_max_epu8(vmax, vh);
			
			// Update E in case it can be improved using our new vh
			vh = _mm_subs_epu8(vh, rdgapo);
			vh = _mm_subs_epu8(vh, *pvScore); // veto some read gap opens
			ve = _mm_max_epu8(ve, vh);
			_mm_store_si128(pvEStore, ve);
			pvEStore += rowstride;
			pvScore += 2;
			
			assert_lt(j, iter);
			if(++j == iter) {
				pvFStore -= colstride;
				vtmp = _mm_load_si128(pvFStore);   // load next vf ASAP
				pvHStore -= colstride;
				vh = _mm_load_si128(pvHStore);     // load next vh ASAP
				pvEStore -= colstride;
				ve = _mm_load_si128(pvEStore);     // load next ve ASAP
				pvScore = d.qprof_ + off + 1;
				j = 0;
				vf = _mm_slli_si128(vf, NBYTES_PER_WORD);
			} else {
				vtmp = _mm_load_si128(pvFStore);   // load next vf ASAP
				vh = _mm_load_si128(pvHStore);     // load next vh ASAP
				ve = _mm_load_si128(pvEStore);     // load next vh ASAP
			}
			
			// Update F with another gap extension
			vf = _mm_subs_epu8(vf, rfgape);
			vf = _mm_subs_epu8(vf, *pvScore); // veto some ref gap extensions
			vf = _mm_max_epu8(vtmp, vf);
			vtmp = _mm_subs_epu8(vf, vtmp);
			vtmp = _mm_cmpeq_epi8(vtmp, vzero);
			cmp = _mm_movemask_epi8(vtmp);
		}

		// pvELoad and pvHLoad are already where they need to be
		
		// Adjust the load and store vectors here.  
		pvHStore = pvHLoad + colstride;
		pvEStore = pvELoad + colstride;
		pvFStore = pvFTmp;
	}

	// Find largest score in vmax
	vtmp = _mm_srli_si128(vmax, 8);
	vmax = _mm_max_epu8(vmax, vtmp);
	vtmp = _mm_srli_si128(vmax, 4);
	vmax = _mm_max_epu8(vmax, vtmp);
	vtmp = _mm_srli_si128(vmax, 2);
	vmax = _mm_max_epu8(vmax, vtmp);
	vtmp = _mm_srli_si128(vmax, 1);
	vmax = _mm_max_epu8(vmax, vtmp);

	int score = _mm_extract_epi16(vmax, 0);
	score = score & 0x00ff;

	// Could we have saturated?
	if(score + d.bias_ >= 255) {
		flag = -2; // yes
		return std::numeric_limits<TAlScore>::min();
	}
	
	// Did we find a solution?
	if(score == std::numeric_limits<TCScore>::min()) {
		flag = -1; // no
		return std::numeric_limits<TAlScore>::min();
	}
	
	// Return largest score
	return (TAlScore)score;
}

/**
 * Given a filled-in DP table, populate the btncand_ list with candidate cells
 * that might be at the ends of valid alignments.  No need to do this unless
 * the maximum score returned by the align*() func is >= the minimum.
 *
 * Only cells that are exhaustively scored are candidates.  Those are the
 * cells inside the shape made of o's in this:
 *
 *  |-maxgaps-|
 *  ***********oooooooooooooooooooooo    -
 *   ***********ooooooooooooooooooooo    |
 *    ***********oooooooooooooooooooo    |
 *     ***********ooooooooooooooooooo    |
 *      ***********oooooooooooooooooo    |
 *       ***********ooooooooooooooooo read len
 *        ***********oooooooooooooooo    |
 *         ***********ooooooooooooooo    |
 *          ***********oooooooooooooo    |
 *           ***********ooooooooooooo    |
 *            ***********oooooooooooo    -
 *            |-maxgaps-|
 *  |-readlen-|
 *  |-------skip--------|
 *
 * And it's possible for the shape to be truncated on the left and right sides.
 *
 * 
 */
bool SwAligner::gatherCellsNucleotidesLocalSseU8(TAlScore best) {
	// What's the minimum number of rows that can possibly be spanned by an
	// alignment that meets the minimum score requirement?
	size_t bonus = sc_->match(30);
	assert_gt(minsc_, 0);
	const size_t ncol = rff_ - rfi_;
	const size_t nrow = dpRows();
	assert_gt(nrow, 0);
	btncand_.clear();
	SSEData& d = fw_ ? sseU8fw_ : sseU8rc_;
	assert(!d.buf_.empty());
	assert(d.qprof_ != NULL);
	const size_t rowstride = d.mat_.rowstride();
	const size_t colstride = d.mat_.colstride();
	size_t iter = (dpRows() + (NWORDS_PER_REG - 1)) / NWORDS_PER_REG;
	assert_gt(iter, 0);
	size_t minrow = ((minsc_ + bonus - 1) / bonus) - 1;
	// Iterate over columns
	size_t skip = maxgaps_; // columns w/r/t un-truncated table to skip
	size_t iskip = skip;    // columns w/r/t table we filled in to skip
	if(truncLeft_ >= maxgaps_) {
		skip += (truncLeft_ - maxgaps_);
		iskip = 0;
	} else {
		iskip -= truncLeft_;
	}
	size_t col  = skip;  // off w/r/t the un-truncated table
	size_t icol = iskip; // off w/r/t the table we filled in
	size_t nrow_thiscol = min<size_t>(col - maxgaps_ + 1, nrow);
	for(size_t j = icol; j < ncol; j++) {
		// Skip cells that en_ tells us to
		size_t fromend = ncol - j - 1;
		if(en_ != NULL && fromend < solwidth_) {
			size_t off = solwidth_ - fromend - 1;
			assert_lt(off, en_->size());
			if(!(*en_)[off]) {
				continue;
			}
		}
		// Get pointer to first cell in column to examine:
		__m128i *pvHorig = d.mat_.hvec(0, j);
		__m128i *pvH     = pvHorig;
		// Get pointer to the vector in the following column that corresponds
		// to the cells diagonally down and to the right from the cells in pvH
		__m128i *pvHSucc = (j < ncol-1) ? d.mat_.hvec(0, j+1) : NULL;
		__m128i *pvHPrevBase = (j > 0)  ? d.mat_.hvec(0, j-1) : NULL;
		__m128i *pvHPrev = NULL;
		size_t succOff = 0;
		size_t prevOff = 0;
		// Start in upper vector row and move down
		for(size_t i = 0; i < iter; i++) {
			if(pvHSucc != NULL) {
				pvHSucc += rowstride;
				if(i == iter-1) {
					pvHSucc = d.mat_.hvec(0, j+1);
					succOff = 1;
				}
			}
			if(pvHPrevBase != NULL) {
				if(i == 0) {
					pvHPrev = pvHPrevBase + colstride - rowstride;
					prevOff = 1;
				} else {
					pvHPrev = pvHPrevBase + (i - 1) * rowstride;
					prevOff = 0;
				}
			}
			// Which elements of this vector are exhaustively scored?
			size_t rdoff = i;
			for(size_t k = 0; k < NWORDS_PER_REG; k++) {
				if(rdoff < nrow_thiscol) {
					// This cell has been exhaustively scored
					if(rdoff >= minrow) {
						// ... and it could potentially score high enough
						TAlScore sc = (TAlScore)((TCScore*)pvH)[k];
						assert_leq(sc, best);
						if(sc >= minsc_) {
							// This is a potential solution
							bool matchSucc = false;
							int readc = (*rd_)[rdoff];
							int refc = rf_[j + rfi_];
							bool match = ((refc & (1 << readc)) != 0);
							if(rdoff < dpRows()-1) {
								int readcSucc = (*rd_)[rdoff+1];
								int refcSucc = rf_[j + rfi_ + 1];
								matchSucc = ((refcSucc & (1 << readcSucc)) != 0);
							}
							if(match && !matchSucc) {
								// Yes, this is legit
								btncand_.expand();
								btncand_.back().init(rdoff, j, sc);
							}
						}
					}
				} else {
					// Already saw every element in the vector that's been
					// exhaustively scored
					break;
				}
				rdoff += iter;
			}
			pvH += rowstride;
		}
		nrow_thiscol = min<size_t>(nrow_thiscol+1, nrow);
	}
	btncand_.sort();
	if(btncand_.empty()) {
		nfail_++;
	} else {
		nsucc_++;
		d.mat_.initMasks();
	}
	return !btncand_.empty();
}

/**
 * Given the dynamic programming table and a cell, trace backwards from the
 * cell and install the edits and score/penalty in the appropriate fields
 * of res.  The RandomSource is used to break ties among equally good ways
 * of tracing back.
 *
 * Whenever we enter a cell, we check whether the read/ref coordinates of
 * that cell correspond to a cell we traversed constructing a previous
 * alignment.  If so, we backtrack to the last decision point, mask out the
 * path that led to the previously observed cell, and continue along a
 * different path; or, if there are no more paths to try, we give up.
 *
 * If an alignment is found, 'off' is set to the alignment's upstream-most
 * reference character's offset into the chromosome and true is returned.
 * Otherwise, false is returned.
 */
bool SwAligner::backtraceNucleotidesLocalSseU8(
	TAlScore       escore, // in: expected score
	SwResult&      res,    // out: store results (edits and scores) here
	size_t&        off,    // out: store diagonal projection of origin
	size_t         row,    // start in this row
	size_t         col,    // start in this column
	RandomSource&  rand)   // random gen, to choose among equal paths
{
	SSEData& d = fw_ ? sseU8fw_ : sseU8rc_;
	assert(!d.buf_.empty());
	assert(d.qprof_ != NULL);
	assert_lt(row, rd_->length());
	btnstack_.clear(); // empty the backtrack stack
	btcells_.clear();  // empty the cells-so-far list
	AlnScore score; score.score_ = 0;
	score.gaps_ = score.ns_ = 0;
	size_t origCol = col;
	size_t gaps = 0, readGaps = 0, refGaps = 0;
	res.alres.reset();
	EList<Edit>& ned = res.alres.ned();
	assert(ned.empty());
	assert_gt(dpRows(), row);
	size_t trimEnd = dpRows() - row - 1; 
	size_t trimBeg = 0;
	size_t ct = d.mat_.H; // cell type
	//cout << "Backtrace from row " << row << endl;
	while((int)row >= 0) {
		nbts_++;
		int readc = (*rd_)[rdi_ + row];
		int refm  = (int)rf_[rfi_ + col];
		int readq = (*qu_)[row];
		assert_leq(col, origCol);
		// Get score in this cell
		bool empty, reportedThru, canMoveThru, branch = false;
		int cur;
		d.mat_.analyzeCell(
			row,           // row
			col,           // column
			ct,            // E/F/H
			refm,          // reference mask
			readc,         // read character
			readq,         // read quality
			*sc_,          // scoring scheme
			0,             // offset to add to each matrix elt
			floorsc_,      // local-alignment floor score
			rand,          // rand gen for choosing among equal options
			empty,         // out: =true iff no way to backtrace
			cur,           // out: =type of transition
			branch,        // out: =true iff we chose among >1 options
			canMoveThru,   // out: =true iff ...
			reportedThru); // out: =true iff ...
		assert_eq(gaps, Edit::numGaps(ned));
		assert_leq(gaps, rdgap_ + rfgap_);
		// Cell was involved in a previously-reported alignment?
		if(!canMoveThru) {
			if(!btnstack_.empty()) {
				// Remove all the cells from list back to and including the
				// cell where the branch occurred
				btcells_.resize(btnstack_.back().celsz);
				// Pop record off the top of the stack
				ned.resize(btnstack_.back().nedsz);
				//aed.resize(btnstack_.back().aedsz);
				row      = btnstack_.back().row;
				col      = btnstack_.back().col;
				gaps     = btnstack_.back().gaps;
				readGaps = btnstack_.back().readGaps;
				refGaps  = btnstack_.back().refGaps;
				score    = btnstack_.back().score;
				ct       = btnstack_.back().ct;
				btnstack_.pop_back();
				assert(!sc_->monotone || score.score() >= escore);
				continue;
			} else {
				// No branch points to revisit; just give up
				res.reset();
				return false;
			}
		}
		assert(!reportedThru);
		assert(!sc_->monotone || score.score() >= minsc_);
		if(empty || row == 0) {
			assert_eq(SSEMatrix::H, ct);
			btcells_.expand();
			btcells_.back().first = row;
			btcells_.back().second = col;
			// This cell is at the end of a legitimate alignment
			trimBeg = row;
			assert_eq(btcells_.size(), dpRows() - trimBeg - trimEnd + readGaps);
			break;
		}
		if(branch) {
			// Add a frame to the backtrack stack
			btnstack_.expand();
			btnstack_.back().init(
				ned.size(),
				0,               // aed.size()
				btcells_.size(),
				row,
				col,
				gaps,
				readGaps,
				refGaps,
				score,
				(int)ct);
		}
		btcells_.expand();
		btcells_.back().first = row;
		btcells_.back().second = col;
		SizeTPair p = make_pair(row, rfi_ + col);
		switch(cur) {
			// Move up and to the left.  If the reference nucleotide in the
			// source row mismatches the read nucleotide, penalize
			// it and add a nucleotide mismatch.
			case SW_BT_OALL_DIAG: {
				assert_gt(row, 0); assert_gt(col, 0);
				// Check for color mismatch
				int readC = (*rd_)[row];
				int refNmask = (int)rf_[rfi_+col];
				assert_gt(refNmask, 0);
				int m = matchesEx(readC, refNmask);
				ct = SSEMatrix::H;
				if(m != 1) {
					Edit e(
						(int)row,
						mask2dna[refNmask],
						"ACGTN"[readC],
						EDIT_TYPE_MM);
					assert(e.repOk());
					assert(ned.empty() || ned.back().pos >= row);
					ned.push_back(e);
					int pen = QUAL2(row, col);
					score.score_ -= pen;
					assert(!sc_->monotone || score.score() >= escore);
				} else {
					// Reward a match
					int64_t bonus = sc_->match(30);
					score.score_ += bonus;
					assert(!sc_->monotone || score.score() >= escore);
				}
				if(m == -1) {
					score.ns_++;
				}
				row--; col--;
				assert(VALID_AL_SCORE(score));
				break;
			}
			// Move up.  Add an edit encoding the ref gap.
			case SW_BT_OALL_REF_OPEN:
			{
				assert_gt(row, 0);
				Edit e(
					(int)row,
					'-',
					"ACGTN"[(int)(*rd_)[row]],
					EDIT_TYPE_REF_GAP);
				assert(e.repOk());
				assert(ned.empty() || ned.back().pos >= row);
				ned.push_back(e);
				assert_geq(row, (size_t)sc_->gapbar);
				assert_geq((int)(rdf_-rdi_-row-1), sc_->gapbar-1);
				row--;
				ct = SSEMatrix::H;
				int pen = sc_->refGapOpen();
				score.score_ -= pen;
				assert(!sc_->monotone || score.score() >= minsc_);
				gaps++; refGaps++;
				assert_eq(gaps, Edit::numGaps(ned));
				assert_leq(gaps, rdgap_ + rfgap_);
				break;
			}
			// Move up.  Add an edit encoding the ref gap.
			case SW_BT_RFGAP_EXTEND:
			{
				assert_gt(row, 1);
				Edit e(
					(int)row,
					'-',
					"ACGTN"[(int)(*rd_)[row]],
					EDIT_TYPE_REF_GAP);
				assert(e.repOk());
				assert(ned.empty() || ned.back().pos >= row);
				ned.push_back(e);
				assert_geq(row, (size_t)sc_->gapbar);
				assert_geq((int)(rdf_-rdi_-row-1), sc_->gapbar-1);
				row--;
				ct = SSEMatrix::F;
				int pen = sc_->refGapExtend();
				score.score_ -= pen;
				assert(!sc_->monotone || score.score() >= minsc_);
				gaps++; refGaps++;
				assert_eq(gaps, Edit::numGaps(ned));
				assert_leq(gaps, rdgap_ + rfgap_);
				break;
			}
			case SW_BT_OALL_READ_OPEN:
			{
				assert_gt(col, 0);
				Edit e(
					(int)row+1,
					mask2dna[(int)rf_[rfi_+col]],
					'-',
					EDIT_TYPE_READ_GAP);
				assert(e.repOk());
				assert(ned.empty() || ned.back().pos >= row);
				ned.push_back(e);
				assert_geq(row, (size_t)sc_->gapbar);
				assert_geq((int)(rdf_-rdi_-row-1), sc_->gapbar-1);
				col--;
				ct = SSEMatrix::H;
				int pen = sc_->readGapOpen();
				score.score_ -= pen;
				assert(!sc_->monotone || score.score() >= minsc_);
				gaps++; readGaps++;
				assert_eq(gaps, Edit::numGaps(ned));
				assert_leq(gaps, rdgap_ + rfgap_);
				break;
			}
			case SW_BT_RDGAP_EXTEND:
			{
				assert_gt(col, 1);
				Edit e(
					(int)row+1,
					mask2dna[(int)rf_[rfi_+col]],
					'-',
					EDIT_TYPE_READ_GAP);
				assert(e.repOk());
				assert(ned.empty() || ned.back().pos >= row);
				ned.push_back(e);
				assert_geq(row, (size_t)sc_->gapbar);
				assert_geq((int)(rdf_-rdi_-row-1), sc_->gapbar-1);
				col--;
				ct = SSEMatrix::E;
				int pen = sc_->readGapExtend();
				score.score_ -= pen;
				assert(!sc_->monotone || score.score() >= minsc_);
				gaps++; readGaps++;
				assert_eq(gaps, Edit::numGaps(ned));
				assert_leq(gaps, rdgap_ + rfgap_);
				break;
			}
			default: throw 1;
		}
	} // while((int)row > 0)
	assert_geq(col, 0);
	assert_eq(SSEMatrix::H, ct);
	// The number of cells in the backtracs should equal the number of read
	// bases after trimming plus the number of gaps
	assert_eq(btcells_.size(), dpRows() - trimBeg - trimEnd + readGaps);
	// Set 'reported' flag on each cell
	for(size_t i = 0; i < btcells_.size(); i++) {
		size_t rw = btcells_[i].first;
		size_t cl = btcells_[i].second;
		assert(!d.mat_.reportedThrough(rw, cl));
		d.mat_.setReportedThrough(rw, cl);
		assert(d.mat_.reportedThrough(rw, cl));
	}
	int readC = (*rd_)[rdi_+row];      // get last char in read
	int refNmask = (int)rf_[rfi_+col]; // get last ref char ref involved in aln
	assert_gt(refNmask, 0);
	int m = matchesEx(readC, refNmask);
	if(m != 1) {
		Edit e((int)row, mask2dna[refNmask], "ACGTN"[readC], EDIT_TYPE_MM);
		assert(e.repOk());
		assert(ned.empty() || ned.back().pos >= row);
		ned.push_back(e);
		score.score_ -= QUAL2(row, col);
		assert_geq(score.score(), minsc_);
	} else {
		score.score_ += sc_->match(30);
	}
	if(m == -1) {
		score.ns_++;
	}
	res.reverse();
	assert(Edit::repOk(ned, (*rd_)));
#ifndef NDEBUG
	size_t gapsCheck = 0;
	for(size_t i = 0; i < ned.size(); i++) {
		if(ned[i].isGap()) gapsCheck++;
	}
	assert_eq(gaps, gapsCheck);
	BTDnaString refstr;
	for(size_t i = col; i <= origCol; i++) {
		refstr.append(firsts5[(int)rf_[rfi_+i]]);
	}
	BTDnaString editstr;
	Edit::toRef((*rd_), ned, editstr, trimBeg, trimEnd);
	if(refstr != editstr) {
		cerr << "Decoded nucleotides and edits don't match reference:" << endl;
		cerr << "           score: " << score.score()
		     << " (" << gaps << " gaps)" << endl;
		cerr << "           edits: ";
		Edit::print(cerr, ned);
		cerr << endl;
		cerr << "    decoded nucs: " << (*rd_) << endl;
		cerr << "     edited nucs: " << editstr << endl;
		cerr << "  reference nucs: " << refstr << endl;
		assert(0);
	}
#endif
	assert_eq(score.score(), escore);
	assert_leq(gaps, rdgap_ + rfgap_);
	off = col;
	assert_lt(col + rfi_, rff_);
	score.gaps_ = gaps;
	res.alres.setScore(score);
	res.alres.setShape(
		refidx_,                  // ref id
		off + rfi_ + refoff_,     // 0-based ref offset
		fw_,                      // aligned to Watson?
		rdf_ - rdi_,              // read length
		color_,                   // read was colorspace?
		true,                     // pretrim soft?
		0,                        // pretrim 5' end
		0,                        // pretrim 3' end
		true,                     // alignment trim soft?
		fw_ ? trimBeg : trimEnd,  // alignment trim 5' end
		fw_ ? trimEnd : trimBeg); // alignment trim 3' end
	size_t refns = 0;
	for(size_t i = col; i <= origCol; i++) {
		if((int)rf_[rfi_+i] > 15) {
			refns++;
		}
	}
	res.alres.setRefNs(refns);
	assert(res.repOk());
	return true;
}

#endif