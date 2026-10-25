/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"      /* Obtains integer types */
#include "diskio.h"    /* Declarations of disk functions */
#include <grub/types.h>
#include <grub/disk.h>
#include <grub/err.h>
#include <grub/datetime.h>

STAT fat_stat[MAX_DRIVES];

/* FatFs uses filesystem BPB sector units, while grub_disk_read/write
   always address 512-byte units.  Discover the BPB size once per mount;
   never infer it solely from the underlying physical disk geometry. */
static grub_uint16_t
fat_sector_size (BYTE pdrv)
{
  grub_uint8_t boot[GRUB_DISK_SECTOR_SIZE];
  grub_uint16_t size = GRUB_DISK_SECTOR_SIZE;
  grub_err_t saved = grub_errno;
  grub_disk_t disk = fat_stat[pdrv].disk;

  if (fat_stat[pdrv].sector_size)
    return fat_stat[pdrv].sector_size;
  if (!disk)
    return size;

  if (grub_disk_read (disk, 0, 0, sizeof (boot), boot) != GRUB_ERR_NONE)
    {
      /* Do not cache 512 bytes after a transient failure to read the
         BPB: a subsequent access might be to a 4K-sector volume. */
      grub_errno = saved;
      return 0;
    }

  if (boot[510] == 0x55 && boot[511] == 0xaa)
    {
      if (boot[3] == 'E' && boot[4] == 'X' && boot[5] == 'F'
          && boot[6] == 'A' && boot[7] == 'T' && boot[8] == ' '
          && boot[9] == ' ' && boot[10] == ' '
          && boot[108] >= 9 && boot[108] <= 12)
        size = (grub_uint16_t) 1U << boot[108];
      else if ((boot[0] == 0xeb || boot[0] == 0xe9 || boot[0] == 0xe8)
               && boot[13] && !(boot[13] & (boot[13] - 1)))
        {
          grub_uint16_t bpb = boot[11] | ((grub_uint16_t) boot[12] << 8);
          if (bpb == 512 || bpb == 1024 || bpb == 2048 || bpb == 4096)
            size = bpb;
        }
    }

  /* A failed probe must not poison a later FatFs operation's error state. */
  grub_errno = saved;
  fat_stat[pdrv].sector_size = size;
  return size;
}

static DRESULT
fat_io_range (BYTE pdrv, LBA_t sector, UINT count,
              grub_disk_addr_t *grub_sector, grub_size_t *size)
{
  grub_uint16_t sector_size = fat_sector_size (pdrv);
  grub_uint64_t factor;
  grub_uint64_t total;

  if (!sector_size)
    return RES_ERROR;
  factor = sector_size >> GRUB_DISK_SECTOR_BITS;
  total = grub_divmod64 (fat_stat[pdrv].total_sectors, factor, NULL);

  if (!count || sector >= total || count > total - sector
      || count > ((grub_size_t) -1) / sector_size)
    return RES_PARERR;

  *grub_sector = (grub_uint64_t) sector * factor;
  *size = (grub_size_t) count * sector_size;
  return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (BYTE pdrv)
{
  if (pdrv >= MAX_DRIVES)
    return STA_NOINIT;

  if (!fat_stat[pdrv].present)
    return STA_NOINIT;
  if (!fat_stat[pdrv].disk)
    return STA_NOINIT;

  return 0;
}

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (BYTE pdrv)
{
  if (pdrv >= MAX_DRIVES)
    return STA_NOINIT;

  if (!fat_stat[pdrv].present)
    return STA_NOINIT;
  if (!fat_stat[pdrv].disk)
    return STA_NOINIT;

  return 0;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
  grub_err_t errno;
  grub_disk_addr_t grub_sector;
  grub_size_t size;

  if (pdrv >= MAX_DRIVES)
    return RES_PARERR;
  if (!fat_stat[pdrv].present || !fat_stat[pdrv].disk)
    return RES_NOTRDY;
  if (fat_io_range (pdrv, sector, count, &grub_sector, &size) != RES_OK)
    return RES_PARERR;

  errno = grub_disk_read (fat_stat[pdrv].disk, grub_sector, 0, size, buff);

  if (errno == GRUB_ERR_NONE)
    return RES_OK;
  return RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

DRESULT disk_write (BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
  grub_err_t errno;
  grub_disk_addr_t grub_sector;
  grub_size_t size;

  if (pdrv >= MAX_DRIVES)
    return RES_PARERR;
  if (!fat_stat[pdrv].present || !fat_stat[pdrv].disk)
    return RES_NOTRDY;
  if (fat_io_range (pdrv, sector, count, &grub_sector, &size) != RES_OK)
    return RES_PARERR;

  errno = grub_disk_write (fat_stat[pdrv].disk, grub_sector, 0, size, buff);

  if (errno == GRUB_ERR_NONE)
    return RES_OK;

  return RES_ERROR;
}

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void *buff)
{
  DRESULT res;
  if (pdrv >= MAX_DRIVES)
    return RES_PARERR;
  if (!fat_stat[pdrv].present)
    return RES_ERROR;
  if (!fat_stat[pdrv].disk)
    return RES_ERROR;

  switch (cmd)
  {
    case CTRL_SYNC:
      res = RES_OK;
      break;
    case GET_SECTOR_COUNT:
      {
        grub_uint16_t ss = fat_sector_size (pdrv);
        if (!ss)
          return RES_ERROR;
        *(LBA_t*)buff = grub_divmod64 (fat_stat[pdrv].total_sectors,
                                      ss >> GRUB_DISK_SECTOR_BITS, NULL);
      }
      res = RES_OK;
      break;
    case GET_SECTOR_SIZE:
      *(WORD*)buff = fat_sector_size (pdrv);
      res = *(WORD*)buff ? RES_OK : RES_ERROR;
      break;
    case GET_BLOCK_SIZE:
      *(DWORD*)buff = 1;
      res = RES_OK;
      break;
    default:
      res = RES_PARERR;
  }
  return res;
}

DWORD get_fattime (void)
{
  struct grub_datetime tm;

  if (grub_get_datetime (&tm))
    return 0;

  /* Pack date and time into a DWORD variable */
  return ((DWORD)(tm.year - 1980) << 25)
         | ((DWORD)tm.month << 21)
         | ((DWORD)tm.day << 16)
         | (WORD)(tm.hour << 11)
         | (WORD)(tm.minute << 5)
         | (WORD)(tm.second >> 1);
}
