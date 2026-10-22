/* libspu's side the game waits on. TRADEOFF: no SPU yet (milestone 7): transfers to its memory complete at once */

/* The SPU's transfer state, which the paging polls with 0 before a sound page's upload and after it, going on when it
 * is nonzero (decomp/src/game/800123F0.c, Nsf_UpdatePageSlot): SpuIsTransferCompleted's use */
#define SPU_TRANSFER_DONE 1

int func_80051328(int mode) {
    (void)mode;
    return SPU_TRANSFER_DONE;
}
