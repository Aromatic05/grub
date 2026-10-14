/* EFI network inspection commands for the native GRUB network stack.  */
#include <grub/dl.h>
#include <grub/command.h>
#include <grub/net.h>
#include <grub/i18n.h>
#include <grub/misc.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_command_t cmd_list_routes, cmd_list_cards, cmd_list_addrs;
static grub_command_t cmd_add_addr, cmd_bootp, cmd_bootp6;

static grub_err_t
add_addr (struct grub_command *cmd __attribute__ ((unused)),
          int argc, char **args)
{
  struct grub_net_card *card;
  grub_net_network_level_address_t addr;
  grub_net_link_level_address_t hwaddr;
  grub_net_interface_flags_t flags = 0;
  struct grub_net_network_level_interface *inf;
  grub_err_t err;
  unsigned int i;

  if (argc == 3)
    return grub_command_execute ("net_add_addr", argc, args);
  if (argc != 4)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
                       N_("three or four arguments expected"));

  FOR_NET_CARDS (card)
    if (grub_strcmp (card->name, args[1]) == 0)
      break;
  if (!card)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("card not found"));

  err = grub_net_resolve_address (args[2], &addr);
  if (err)
    return err;

  /* Parse exactly six colon-separated hexadecimal octets.  */
  if (grub_strlen (args[3]) != 17)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid hardware address"));
  for (i = 0; i < 6; i++)
    {
      unsigned int hi, lo;
      char c = args[3][i * 3];
      char d = args[3][i * 3 + 1];
      if (c >= '0' && c <= '9') hi = c - '0';
      else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
      else return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid hardware address"));
      if (d >= '0' && d <= '9') lo = d - '0';
      else if (d >= 'a' && d <= 'f') lo = d - 'a' + 10;
      else if (d >= 'A' && d <= 'F') lo = d - 'A' + 10;
      else return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid hardware address"));
      hwaddr.mac[i] = (hi << 4) | lo;
      if (i < 5 && args[3][i * 3 + 2] != ':')
        return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("invalid hardware address"));
    }
  if (card->flags & GRUB_NET_CARD_NO_MANUAL_INTERFACES)
    return grub_error (GRUB_ERR_IO,
                       N_("this card doesn't support address addition"));
  if (card->flags & GRUB_NET_CARD_HWADDRESS_IMMUTABLE)
    {
      if (grub_net_hwaddr_cmp (&hwaddr, &card->default_address) != 0)
        return grub_error (GRUB_ERR_IO,
                           N_("this card's hardware address is immutable"));
      flags |= GRUB_NET_INTERFACE_HWADDRESS_IMMUTABLE;
    }

  inf = grub_net_add_addr (args[0], card, &addr, &hwaddr, flags);
  if (!inf)
    return grub_errno;
  if (addr.type == GRUB_NET_NETWORK_LEVEL_PROTOCOL_IPV4)
    {
      err = grub_net_add_ipv4_local (inf, -1);
      if (err)
        return err;
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
bootp (struct grub_command *cmd __attribute__ ((unused)),
       int argc, char **args)
{
  if (argc > 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("zero or one argument expected"));
  return grub_command_execute ("net_dhcp", argc, args);
}

static grub_err_t
bootp6 (struct grub_command *cmd __attribute__ ((unused)),
        int argc, char **args)
{
  if (argc > 1)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("zero or one argument expected"));
  return grub_command_execute ("net_bootp6", argc, args);
}

static grub_err_t
list_routes (struct grub_command *cmd __attribute__ ((unused)),
             int argc,
             char **args __attribute__ ((unused)))
{
  struct grub_net_route *route;

  if (argc != 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("no arguments expected"));
  for (route = grub_net_routes; route; route = route->next)
    {
      grub_net_network_level_address_t address;
      char text[GRUB_NET_MAX_STR_ADDR_LEN];
      address.type = route->target.type;
      if (address.type == GRUB_NET_NETWORK_LEVEL_PROTOCOL_IPV4)
        address.ipv4 = route->target.ipv4.base;
      else
        {
          address.ipv6[0] = route->target.ipv6.base[0];
          address.ipv6[1] = route->target.ipv6.base[1];
        }
      grub_net_addr_to_str (&address, text);
      grub_printf ("%s %s/%d %s\n", route->name, text,
                   address.type == GRUB_NET_NETWORK_LEVEL_PROTOCOL_IPV4
                     ? route->target.ipv4.masksize : route->target.ipv6.masksize,
                   route->interface ? route->interface->name : "gateway");
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
list_cards (struct grub_command *cmd __attribute__ ((unused)),
            int argc,
            char **args __attribute__ ((unused)))
{
  struct grub_net_card *card;

  if (argc != 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("no arguments expected"));
  for (card = grub_net_cards; card; card = card->next)
    {
      char address[GRUB_NET_MAX_STR_HWADDR_LEN];
      grub_net_hwaddr_to_str (&card->default_address, address);
      grub_printf ("%s %s\n", card->name, address);
    }
  return GRUB_ERR_NONE;
}

static grub_err_t
list_addrs (struct grub_command *cmd __attribute__ ((unused)),
            int argc,
            char **args __attribute__ ((unused)))
{
  struct grub_net_network_level_interface *inf;

  if (argc != 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("no arguments expected"));
  for (inf = grub_net_network_level_interfaces; inf; inf = inf->next)
    {
      char hw[GRUB_NET_MAX_STR_HWADDR_LEN];
      char address[GRUB_NET_MAX_STR_ADDR_LEN];
      grub_net_hwaddr_to_str (&inf->hwaddress, hw);
      grub_net_addr_to_str (&inf->address, address);
      grub_printf ("%s %s %s\n", inf->name, hw, address);
    }
  return GRUB_ERR_NONE;
}

GRUB_MOD_INIT (efi_netfs)
{
  cmd_list_routes = grub_register_command ("net_efi_ls_routes", list_routes,
                                            "", N_("list EFI network routes"));
  cmd_list_cards = grub_register_command ("net_efi_ls_cards", list_cards,
                                          "", N_("list EFI network cards"));
  cmd_list_addrs = grub_register_command ("net_efi_ls_addr", list_addrs,
                                           "", N_("list EFI network addresses"));
  cmd_add_addr = grub_register_command ("net_efi_add_addr", add_addr,
                                         N_("SHORTNAME CARD ADDRESS [HWADDRESS]"),
                                         N_("add a network address"));
  cmd_bootp = grub_register_command ("net_efi_bootp", bootp,
                                      N_("[CARD]"),
                                      N_("perform a DHCPv4 autoconfiguration"));
  cmd_bootp6 = grub_register_command ("net_efi_bootp6", bootp6,
                                       N_("[CARD]"),
                                       N_("perform a DHCPv6 autoconfiguration"));
}

GRUB_MOD_FINI (efi_netfs)
{
  grub_unregister_command (cmd_list_routes);
  grub_unregister_command (cmd_list_cards);
  grub_unregister_command (cmd_list_addrs);
  grub_unregister_command (cmd_add_addr);
  grub_unregister_command (cmd_bootp);
  grub_unregister_command (cmd_bootp6);
}
