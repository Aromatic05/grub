/*
 * Load the modern BLS/UKI module when a1ive configurations request the
 * historical blscfg module name.  The actual blscfg and bls_import
 * commands are registered by blsuki.
 */

#include <grub/dl.h>

GRUB_MOD_LICENSE ("GPLv3+");

GRUB_MOD_INIT (blscfg)
{
  /* The legacy module name must load the implementation rather than merely
     registering an unused compatibility shell.  No static blsuki symbol is
     referenced, so this is a runtime dependency, not a .moddeps record. */
  grub_dl_load ("blsuki");
}

GRUB_MOD_FINI (blscfg)
{
}
