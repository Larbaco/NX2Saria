#ifndef ACTION_PROMPT_H
#define ACTION_PROMPT_H

#include <stddef.h>
#include <stdint.h>

#define ACTION_PROMPT_ARCHIVE "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"

int action_prompt_lay_out(uint8_t *p, size_t n, int label, char *why, size_t why_n);

int action_prompt_archive(uint8_t *sarc, size_t n, int label, char *why, size_t why_n);

int action_prompt_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, int label, void **out,
                        size_t *out_n);

#endif
