/* Optional observers of successful checksum results. */
#ifndef GRUB_HASHSUM_H
#define GRUB_HASHSUM_H 1

#include <grub/types.h>
#include <grub/symbol.h>

typedef void (*grub_hashsum_result_hook_t)
  (const grub_uint8_t *digest, grub_size_t size);

void EXPORT_FUNC(grub_hashsum_register_result_hook)
  (grub_hashsum_result_hook_t hook);
void EXPORT_FUNC(grub_hashsum_unregister_result_hook)
  (grub_hashsum_result_hook_t hook);

#endif
