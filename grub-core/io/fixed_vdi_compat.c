/*
 * Historical a1ive module name for the fixed-VDI image reader.
 *
 * The functional VDI parser is registered by the vhd module's ordered disk
 * pipeline.  Loading that implementation here preserves the old insmod name
 * without installing a second, competing file filter.
 */

#include <grub/dl.h>

GRUB_MOD_LICENSE ("GPLv3+");

GRUB_MOD_INIT (fixed_vdi)
{
  grub_dl_load ("vhd");
}

GRUB_MOD_FINI (fixed_vdi)
{
}
