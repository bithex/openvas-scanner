/* OpenVAS
* $Id$
* Description: Basically creates a new process for each tested host.
*
* Authors: - Renaud Deraison <deraison@nessus.org> (Original pre-fork develoment)
*          - Tim Brown <mailto:timb@openvas.org> (Initial fork)
*          - Laban Mwangi <mailto:labanm@openvas.org> (Renaming work)
*          - Tarik El-Yassem <mailto:tarik@openvas.org> (Headers section)
*
* Copyright:
* Portions Copyright (C) 2006 Software in the Public Interest, Inc.
* Based on work Copyright (C) 1998 - 2006 Tenable Network Security, Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2,
* as published by the Free Software Foundation
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <errno.h>    /* for errno() */
#include <sys/wait.h> /* for waitpid() */
#include <string.h>   /* for strlen() */
#include <unistd.h>   /* for close() */

#include <glib.h>     /* for g_free() */

#include "../misc/network.h"      /* for internal_recv */

#include "utils.h" /* for data_left() */
#include "hosts.h" /* for hosts_new() */
#include "ntp.h"   /* for ntp_parse_input() */

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "sd   main"

/**
 * @brief Host information, implemented as doubly linked list.
 */
struct host
{
  char *name;
  pid_t pid;
  kb_t host_kb;
  struct host *next;
  struct host *prev;
};
/** @TODO struct hosts could be stripped down and put in a g_list, or,
 *        as a g_hash_table (name -> [soc,pid]), see hosts_get.*/


static struct host *hosts = NULL;
static int g_soc = -1;
static int g_max_hosts = 15;


/*-------------------------------------------------------------------------*/
static int
forward (kb_t kb, int out)
{
  char *buf = NULL;
  int len = 0;

  while (1)
    {
      buf = kb_item_pop_str (kb, "internal/forward");
      if (!buf)
        return 0;
      len = strlen (buf);
      if (out > 0)
        {
          int n;
          for (n = 0; n < len;)
            {
              int e;
              e = nsend (out, buf + n, len - n, 0);
              if (e < 0 && errno == EINTR)
                continue;
              else if (e < 0)
                {
                  g_free (buf);
                  return -1;
                }
              else
                n += e;
            }
        }
      g_free (buf);
    }

  return 1;
}

/*-------------------------------------------------------------------*/


static void
host_rm (struct host *h)
{
  if (h->pid != 0)
    waitpid (h->pid, NULL, WNOHANG);

  while (forward (h->host_kb, g_soc) > 0)
    ;
  if (h->next != NULL)
    h->next->prev = h->prev;

  if (h->prev != NULL)
    h->prev->next = h->next;

  g_free (h->name);
  kb_delete (h->host_kb);
  g_free (h);
}

/*-----------------------------------------------------------------*/

/**
 * @brief Returns the number of entries in the global hosts list.
 */
static int
hosts_num (void)
{
  struct host *h = hosts;
  int num;

  for (num = 0; h != NULL; num++, h = h->next);

  return num;
}

/**
 * @brief Retrieves a host specified by its name from the global host list.
 */
static struct host *
hosts_get (char *name)
{
  struct host *h = hosts;
  while (h != NULL)
    {
      if (strcmp (h->name, name) == 0)
        return h;
      h = h->next;
    }
  return NULL;
}


int
hosts_init (int soc, int max_hosts)
{
  g_soc = soc;
  g_max_hosts = max_hosts;
  return 0;
}

extern int global_scan_stop;

int
hosts_new (struct scan_globals *globals, char *name, kb_t kb)
{
  struct host *h;

  while (hosts_num () >= g_max_hosts)
    {
      if (hosts_read (globals) < 0)
        return -1;
    }
  if (global_scan_stop)
    return 0;

  h = g_malloc0 (sizeof (struct host));
  h->name = g_strdup (name);
  h->pid = 0;
  h->host_kb = kb;
  if (hosts != NULL)
    hosts->prev = h;
  h->next = hosts;
  h->prev = NULL;
  hosts = h;
  return 0;
}


int
hosts_set_pid (char *name, pid_t pid)
{
  struct host *h = hosts_get (name);
  if (h == NULL)
    {
      g_debug ("host_set_pid() failed!\n");
      return -1;
    }

  h->pid = pid;
  return 0;
}

/*-----------------------------------------------------------------*/
static int
hosts_stop_host (struct host *h)
{
  if (h == NULL)
    return -1;

  g_message ("Stopping host %s scan", h->name);
  kill (h->pid, SIGUSR1);
  return 0;
}

void
hosts_stop_all (void)
{
  struct host *host = hosts;

  global_scan_stop = 1;
  while (host)
    {
      hosts_stop_host (host);
      host = host->next;
    }
}

/*-----------------------------------------------------------------*/

static void
hosts_read_data (void)
{
  struct host *h = hosts;

  waitpid (-1, NULL, WNOHANG);

  if (h == NULL)
    return;

  while (h != NULL)
    {
      if (kill (h->pid, 0) < 0) /* Process is dead */
        {
          if (!h->prev)
            hosts = hosts->next;
          host_rm (h);
          h = hosts;
          if (!h)
            break;
        }
      h = h->next;
    }
  h = hosts;
  while (h)
    {
      forward (h->host_kb, g_soc);
      h = h->next;
    }
}

/**
 * Returns -1 if no socket, error or client asked to stop tests, 0 otherwise.
 */
static int
hosts_read_client (struct scan_globals *globals)
{
  struct timeval tv;
  int e = 0;
  fd_set rd;

  if (g_soc == -1)
    return 0;


  FD_ZERO (&rd);
  FD_SET (g_soc, &rd);

  for (;;)
    {
      tv.tv_sec = 0;
      tv.tv_usec = 1000;
      e = select (g_soc + 1, &rd, NULL, NULL, &tv);
      if (e < 0 && errno == EINTR)
        continue;
      else
        break;
    }

  if (e > 0 && FD_ISSET (g_soc, &rd) != 0)
    {
      int result;
      char buf[4096];

      result = recv_line (g_soc, buf, sizeof (buf) - 1);
      if (result <= 0)
        return -1;
      result = ntp_parse_input (globals, buf);
      if (result == -1)
        return -1;
    }

  return 0;
}

/**
 * @brief Returns -1 if client asked to stop all tests or connection was lost or error.
 *        0 otherwise.
 */
int
hosts_read (struct scan_globals *globals)
{
  if (hosts_read_client (globals) < 0)
    {
      hosts_stop_all ();
      g_debug ("Client abruptly closed the communication");
      return -1;
    }

  if (hosts == NULL)
    return -1;

  hosts_read_data ();
  usleep (500000);

  return 0;
}
