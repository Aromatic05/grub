/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2010,2011  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GRUB_NET_TCP_HEADER
#define GRUB_NET_TCP_HEADER	1
#include <grub/types.h>
#include <grub/net.h>

struct grub_net_tcp_socket;
typedef struct grub_net_tcp_socket *grub_net_tcp_socket_t;

struct grub_net_tcp_listen;
typedef struct grub_net_tcp_listen *grub_net_tcp_listen_t;

/* Optional connect-phase cancellation.  The callback should return true
   after setting grub_errno.  The ordinary open() retains legacy behavior. */
grub_net_tcp_socket_t
grub_net_tcp_open_cancelable (char *server,
                           grub_uint16_t out_port,
                           grub_err_t (*recv_hook) (grub_net_tcp_socket_t sock,
                                                   struct grub_net_buff *nb,
                                                   void *data),
                           void (*error_hook) (grub_net_tcp_socket_t sock,
                                               void *data),
                           void (*fin_hook) (grub_net_tcp_socket_t sock,
                                             void *data),
                           void *hook_data,
                           int (*cancel) (void *), void *cancel_data);

grub_net_tcp_socket_t
grub_net_tcp_open (char *server,
		   grub_uint16_t out_port,
		   grub_err_t (*recv_hook) (grub_net_tcp_socket_t sock,
					    struct grub_net_buff *nb,
					    void *data),
		   void (*error_hook) (grub_net_tcp_socket_t sock,
				       void *data),
		   void (*fin_hook) (grub_net_tcp_socket_t sock,
				     void *data),
		   void *hook_data);

grub_net_tcp_listen_t
grub_net_tcp_listen (grub_uint16_t port,
		     const struct grub_net_network_level_interface *inf,
		     grub_err_t (*listen_hook) (grub_net_tcp_listen_t listen,
						grub_net_tcp_socket_t sock,
						void *data),
		     void *hook_data);

void
grub_net_tcp_stop_listen (grub_net_tcp_listen_t listen);

grub_err_t
grub_net_send_tcp_packet (const grub_net_tcp_socket_t socket,
			  struct grub_net_buff *nb,
			  int push);

enum
  {
    GRUB_NET_TCP_CONTINUE_RECEIVING,
    GRUB_NET_TCP_DISCARD,
    GRUB_NET_TCP_ABORT
  };

void
grub_net_tcp_close (grub_net_tcp_socket_t sock, int discard_received);

/* Reclaim a socket and all its TCP-owned queues immediately.  The caller must
   stop using it and must not invoke this inside one of its TCP callbacks.
   Unlike close(), disposal does not wait for retransmissions/FIN ACKs. */
void
grub_net_tcp_dispose (grub_net_tcp_socket_t sock);

grub_err_t
grub_net_tcp_accept (grub_net_tcp_socket_t sock,
		     grub_err_t (*recv_hook) (grub_net_tcp_socket_t sock,
					      struct grub_net_buff *nb,
					      void *data),
		     void (*error_hook) (grub_net_tcp_socket_t sock,
					 void *data),
		     void (*fin_hook) (grub_net_tcp_socket_t sock,
				       void *data),
		     void *hook_data);

void
grub_net_tcp_stall (grub_net_tcp_socket_t sock);

void
grub_net_tcp_unstall (grub_net_tcp_socket_t sock);

#endif
