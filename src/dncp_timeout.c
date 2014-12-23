/*
 * $Id: dncp_timeout.c $
 *
 * Author: Markus Stenberg <mstenber@cisco.com>
 *
 * Copyright (c) 2013 cisco Systems, Inc.
 *
 * Created:       Tue Nov 26 08:28:59 2013 mstenber
 * Last modified: Tue Dec 23 15:11:49 2014 mstenber
 * Edit time:     389 min
 *
 */

#include "dncp_i.h"

static void trickle_set_i(hncp_link l, int i)
{
  hnetd_time_t now = hncp_time(l->hncp);
  int imin = l->conf->trickle_imin;
  if (!imin) imin = DNCP_TRICKLE_IMIN;
  int imax = l->conf->trickle_imax;
  if (!imax) imax = DNCP_TRICKLE_IMAX;

  i = i < imin ? imin : i > imax ? imax : i;
  l->trickle_i = i;
  int t = i / 2 + random() % (i / 2);
  l->trickle_send_time = now + t;
  l->trickle_interval_end_time = now + i;
  l->trickle_c = 0;
  L_DEBUG(DNCP_LINK_F " trickle set to %d/%d", DNCP_LINK_D(l), t, i);
}

static void trickle_upgrade(hncp_link l)
{
  trickle_set_i(l, l->trickle_i * 2);
}

static void trickle_send_nocheck(hncp_link l)
{
  l->num_trickle_sent++;
  l->last_trickle_sent = hncp_time(l->hncp);
  dncp_profile_link_send_network_state(l);
  if (l->conf->keepalive_interval)
    l->next_keepalive_time = l->last_trickle_sent + l->conf->keepalive_interval;
}

static void trickle_send(hncp_link l)
{
  if (l->trickle_c < l->conf->trickle_k)
    {
      trickle_send_nocheck(l);
    }
  else
    {
      l->num_trickle_skipped++;
      L_DEBUG(DNCP_LINK_F " trickle already has c=%d >= k=%d, not sending",
              DNCP_LINK_D(l), l->trickle_c, l->conf->trickle_k);
    }
  l->trickle_send_time = 0;
}

static void _node_set_reachable(hncp_node n, bool value)
{
  hncp o = n->hncp;
  bool is_reachable = o->last_prune == n->last_reachable_prune;

  if (is_reachable != value)
    {
      o->network_hash_dirty = true;

      if (!value)
        hncp_notify_subscribers_tlvs_changed(n, n->tlv_container_valid, NULL);

      hncp_notify_subscribers_node_changed(n, value);

      if (value)
        hncp_notify_subscribers_tlvs_changed(n, NULL, n->tlv_container_valid);
    }
  if (value)
    n->last_reachable_prune = hncp_time(o);
}

static void hncp_prune_rec(hncp_node n)
{
  struct tlv_attr *tlvs, *a;
  hncp_t_node_data_neighbor ne;
  hncp_node n2;

  if (!n)
    return;

  /* Stop the iteration if we're already added to current
   * generation. */
  if (n->in_nodes.version == n->hncp->nodes.version)
    return;

  tlvs = hncp_node_get_tlvs(n);

  L_DEBUG("hncp_prune_rec %s / %p = %p",
          DNCP_NODE_REPR(n), n, tlvs);

  /* No TLVs? No point recursing, unless the node is us (we have to
   * visit it always in any case). */
  if (!tlvs && n != n->hncp->own_node)
    return;

  /* Refresh the entry - we clearly did reach it. */
  vlist_add(&n->hncp->nodes, &n->in_nodes, n);
  _node_set_reachable(n, true);

  /* Look at it's neighbors. */
  tlv_for_each_attr(a, tlvs)
    if ((ne = hncp_tlv_neighbor(a)))
      {
        /* Ignore if it's not _bidirectional_ neighbor. Unidirectional
         * ones lead to graph not settling down. */
        if ((n2 = hncp_node_find_neigh_bidir(n, ne)))
          hncp_prune_rec(n2);
      }
}

static void hncp_prune(hncp o)
{
  hnetd_time_t now = hncp_time(o);
  hnetd_time_t grace_after = now - DNCP_PRUNE_GRACE_PERIOD;

  /* Logic fails if time isn't moving forward-ish */
  assert(now != o->last_prune);

  L_DEBUG("hncp_prune %p", o);

  /* Prune the node graph. IOW, start at own node, flood fill, and zap
   * anything that didn't seem appropriate. */
  vlist_update(&o->nodes);

  hncp_prune_rec(o->own_node);

  hncp_node n;
  hnetd_time_t next_time = 0;
  vlist_for_each_element(&o->nodes, n, in_nodes)
    {
      if (n->in_nodes.version == o->nodes.version)
        continue;
      if (n->last_reachable_prune < grace_after)
        continue;
      next_time = TMIN(next_time,
                       n->last_reachable_prune + DNCP_PRUNE_GRACE_PERIOD + 1);
      vlist_add(&o->nodes, &n->in_nodes, n);
      _node_set_reachable(n, false);
    }
  o->next_prune = next_time;
  vlist_flush(&o->nodes);
  o->last_prune = now;
}

void hncp_run(hncp o)
{
  hnetd_time_t next = 0;
  hnetd_time_t now = hncp_io_time(o);
  hncp_link l;
  hncp_tlv t, t2;

  /* Assumption: We're within RTC step here -> can use same timestamp
   * all the way. */
  o->now = now;

  /* If we weren't before, we are now processing within timeout (no
   * sense scheduling extra timeouts within hncp_self_flush or hncp_prune). */
  o->immediate_scheduled = true;

  /* Handle the own TLV roll-over first. */
  if (!o->tlvs_dirty)
    {
      next = o->own_node->origination_time + (1LL << 32) - (1LL << 16);
      if (next <= now)
        {
          o->republish_tlvs = true;
          next = 0;
        }
    }

  /* Refresh locally originated data; by doing this, we can avoid
   * replicating code. */
  hncp_self_flush(o->own_node);

  if (!o->disable_prune)
    {
      if (o->graph_dirty)
        o->next_prune = DNCP_MINIMUM_PRUNE_INTERVAL + o->last_prune;

      if (o->next_prune && o->next_prune <= now)
        {
          o->graph_dirty = false;
          hncp_prune(o);
        }

      /* next_prune may be set _by_ hncp_prune, therefore redundant
       * looking check */
      next = TMIN(next, o->next_prune);
    }

  /* Release the flag to allow more change-triggered zero timeouts to
   * be scheduled. (We don't want to do this before we're done with
   * our mutations of state that can be addressed by the ordering of
   * events within hncp_run). */
  o->immediate_scheduled = false;

  /* First off: If the network hash is dirty, recalculate it (and hope
   * the outcome ISN'T). */
  if (o->network_hash_dirty)
    {
      /* Store original network hash for future study. */
      hncp_hash_s old_hash = o->network_hash;

      hncp_calculate_network_hash(o);
      if (memcmp(&old_hash, &o->network_hash, DNCP_HASH_LEN))
        {
          /* Shocker. The network hash changed -> reset _every_
           * trickle (that is actually running; join_pending ones
           * don't really count). */
          vlist_for_each_element(&o->links, l, in_links)
            trickle_set_i(l, l->conf->trickle_imin);
        }
    }

  vlist_for_each_element(&o->links, l, in_links)
    {
      /* If we're in join pending state, we retry every
       * DNCP_REJOIN_INTERVAL if necessary. */
      if (l->join_failed_time)
        {
          hnetd_time_t next_time =
            l->join_failed_time + DNCP_REJOIN_INTERVAL;
          if (next_time <= now)
            {
              if (!hncp_io_set_ifname_enabled(o, l->ifname, true))
                {
                  l->join_failed_time = now;
                }
              else
                {
                  l->join_failed_time = 0;

                  /* This is essentially second-stage init for a
                   * link. Before multicast join succeeds, it is
                   * essentially zombie. */
                  if (l->conf->keepalive_interval)
                    l->next_keepalive_time =
                      hncp_time(l->hncp) + l->conf->keepalive_interval;
                  trickle_set_i(l, l->conf->trickle_imin);
                }
            }
          /* If still join pending, do not use this for anything. */
          if (l->join_failed_time)
            {
              /* join_failed_time may have changed.. */
              hnetd_time_t next_time =
                l->join_failed_time + DNCP_REJOIN_INTERVAL;
              next = TMIN(next, next_time);
              continue;
            }
        }

      if (l->trickle_interval_end_time <= now)
        trickle_upgrade(l);
      else if (l->trickle_send_time && l->trickle_send_time <= now)
        trickle_send(l);
      else if (l->next_keepalive_time && l->next_keepalive_time <= now)
        {
          L_DEBUG("sending keep-alive");
          trickle_send_nocheck(l);
          /* Do not increment Trickle i, but set next t to i/2 .. i */
          trickle_set_i(l, l->trickle_i);
        }
      next = TMIN(next, l->trickle_interval_end_time);
      next = TMIN(next, l->trickle_send_time);
      next = TMIN(next, l->next_keepalive_time);
    }

  /* Look at neighbors we should be worried about.. */
  /* vlist_for_each_element(&l->neighbors, n, in_neighbors) */
  hncp_for_each_local_tlv_safe(o, t, t2)
    if (tlv_id(&t->tlv) == DNCP_T_NODE_DATA_NEIGHBOR)
      {
        hncp_neighbor n = hncp_tlv_get_extra(t);
        hnetd_time_t next_time;

        next_time = n->last_sync +
          n->keepalive_interval * DNCP_KEEPALIVE_MULTIPLIER;

        /* No cause to do anything right now. */
        if (next_time > now)
          {
            next = TMIN(next, next_time);
            continue;
          }

        /* Zap the neighbor */
        L_DEBUG(DNCP_NEIGH_F " gone on " DNCP_LINK_F,
                DNCP_NEIGH_D(n), DNCP_LINK_D(l));
        hncp_remove_tlv(o, t);
    }

  if (next && !o->immediate_scheduled)
    hncp_io_schedule(o, next - now);

  /* Clear the cached time, it's most likely no longer valid. */
  o->now = 0;
}