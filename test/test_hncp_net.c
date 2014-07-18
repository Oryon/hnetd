/*
 * $Id: test_hncp_net.c $
 *
 * Author: Markus Stenberg <mstenber@cisco.com>
 *
 * Copyright (c) 2013 cisco Systems, Inc.
 *
 * Created:       Wed Nov 27 10:41:56 2013 mstenber
 * Last modified: Thu Jun 19 17:52:07 2014 mstenber
 * Edit time:     397 min
 *
 */

/*
 * This is N-node version of the testsuite which leverages net_sim.h.
 */

#include <unistd.h>
#include "hncp_trust.h"

/* Test utilities */
#include "net_sim.h"
#include "sput.h"

int log_level = LOG_DEBUG;

/********************************************************* Mocked interfaces */

int pa_update_eap(net_node node, const struct prefix *prefix,
                  const struct pa_rid *rid,
                  const char *ifname, bool to_delete)
{
  sput_fail_unless(prefix, "prefix set");
  sput_fail_unless(rid, "rid set");
  node->updated_eap++;
  return 0;
}

int pa_update_edp(net_node node, const struct prefix *prefix,
                  const struct pa_rid *rid,
                  hnetd_time_t valid_until, hnetd_time_t preferred_until,
                  const void *dhcpv6_data, size_t dhcpv6_len)
{
  sput_fail_unless(prefix, "prefix set");
  sput_fail_unless(rid, "rid set");
  node->updated_edp++;
  return 0;
}

int pa_update_eaa(net_node node, const struct in6_addr *addr,
					const struct pa_rid *rid,
					const char *ifname, bool to_delete)
{return 0;}

/**************************************************************** Test cases */

struct prefix p1 = {
  .prefix = { .s6_addr = {
      0x20, 0x01, 0x00, 0x01}},
  .plen = 54 };

struct prefix p2 = {
  .prefix = { .s6_addr = {
      0x20, 0x02, 0x00, 0x01}},
  .plen = 54 };

void hncp_two(void)
{
  net_sim_s s;
  hncp n1;
  hncp n2;
  hncp_link l1;
  hncp_link l2;
  net_node node1, node2;

  net_sim_init(&s);
  n1 = net_sim_find_hncp(&s, "n1");
  n2 = net_sim_find_hncp(&s, "n2");
  l1 = net_sim_hncp_find_link_by_name(n1, "eth0");
  l2 = net_sim_hncp_find_link_by_name(n2, "eth1");
  sput_fail_unless(avl_is_empty(&l1->neighbors.avl), "no l1 neighbors");
  sput_fail_unless(avl_is_empty(&l2->neighbors.avl), "no l2 neighbors");

  /* connect l1+l2 -> should converge at some point */
  net_sim_set_connected(l1, l2, true);
  net_sim_set_connected(l2, l1, true);
  SIM_WHILE(&s, 100, !net_sim_is_converged(&s));

  hncp_trust_begin_friend_search(n1, 0);
  hncp_trust_begin_friend_search(n2, 0);

  sput_fail_unless(n1->nodes.avl.count == 2, "n1 nodes == 2");
  sput_fail_unless(n2->nodes.avl.count == 2, "n2 nodes == 2");

  /* Play with the prefix API. Feed in stuff! */
  node1 = container_of(n1, net_node_s, n);
  node2 = container_of(n2, net_node_s, n);


  /* First, fake delegated prefixes */
  pa_update_ldp(&node1->pa_data, &p1, "eth0", hnetd_time() + 123, hnetd_time() + 1, NULL, 0);
  pa_update_ldp(&node1->pa_data, &p2, NULL, hnetd_time() + 123, hnetd_time() + 1, NULL, 0);


  SIM_WHILE(&s, 1000,
            node2->updated_edp != 2);

  /* Then fake prefix assignment */
  p1.plen = 64;
  p2.plen = 64;
  pa_update_lap(&node1->pa_data, &p1, "eth0", false);
  pa_update_lap(&node1->pa_data, &p2, NULL, false);
  SIM_WHILE(&s, 1000,
            node2->updated_eap != 2);

  sput_fail_unless(hncp_if_has_highest_id(n1, "eth0") !=
                   hncp_if_has_highest_id(n2, "eth1"),
                   "someone is highest");

  /* disconnect on one side (=> unidirectional traffic) => should at
   * some point disappear. */
  net_sim_set_connected(l1, l2, false);
  SIM_WHILE(&s, 1000,
            !avl_is_empty(&l2->neighbors.avl));

  /* n1 will keep getting stuff from n2, so it's sometimes alive,
   * sometimes not.. However, network hashes should be again
   * different. */
  sput_fail_unless(memcmp(&n1->network_hash, &n2->network_hash, HNCP_HASH_LEN),
                   "hashes different");

  /* Should also have done the necessary purging of nodes due to lack
   * of reachability.. */
  sput_fail_unless(n2->nodes.avl.count == 1, "n2 nodes == 1");

  sput_fail_unless(hncp_if_has_highest_id(n1, "eth0") &&
                   hncp_if_has_highest_id(n2, "eth1"),
                   "both highest");

  sput_fail_unless(hncp_if_has_highest_id(n1, "nonexistent"),
                   "nonexistent highest too");

  net_sim_uninit(&s);
}

/* 11 nodes represented, wired according to how they are wired in the
 * test topology. */
char *nodenames[] = {"cpe", "b1", "b2", "b3", "b4", "b5", "b6",
                     "b7", "b8", "b9", "b10", NULL};
typedef struct {
  int src;
  char *srclink;
  int dst;
  char *dstlink;
} nodeconnection_s;

nodeconnection_s nodeconnections[] = {
  {0, "eth1", 1, "eth0"},
  {0, "eth1", 2, "eth0"},
  {1, "eth1", 5, "eth0"},
  {1, "eth2", 2, "eth1"},
  {1, "eth3", 9, "eth0"},
  {2, "eth2", 3, "eth0"},
  {3, "eth1", 4, "eth0"},
  {4, "eth1", 8, "eth0"},
  {4, "eth1", 9, "eth1"},
  {5, "eth1", 6, "eth0"},
  {6, "eth1", 9, "eth2"},
  {6, "eth2", 7, "eth0"},
  {7, "eth1", 10, "eth0"},
  {8, "eth1", 10, "eth1"},
  {9, "eth3", 10, "eth2"},
};

static void handle_connections(net_sim s,
                               nodeconnection_s *c,
                               int n_conns)
{
  int i;

  for (i = 0 ; i < n_conns ; i++)
    {
      hncp n1 = net_sim_find_hncp(s, nodenames[c->src]);
      hncp_link l1 = net_sim_hncp_find_link_by_name(n1, c->srclink);
      hncp n2 = net_sim_find_hncp(s, nodenames[c->dst]);
      hncp_link l2 = net_sim_hncp_find_link_by_name(n2, c->dstlink);

      net_sim_set_connected(l1, l2, true);
      net_sim_set_connected(l2, l1, true);
      c++;
    }
}

static void raw_bird14(net_sim s)
{
  int num_connections = sizeof(nodeconnections) / sizeof(nodeconnections[0]);

  handle_connections(s, &nodeconnections[0], num_connections);

  SIM_WHILE(s, 10000, !net_sim_is_converged(s));

  sput_fail_unless(net_sim_find_hncp(s, "b10")->nodes.avl.count == 11,
                   "b10 enough nodes");

  sput_fail_unless(hnetd_time() - s->start < 10 * HNETD_TIME_PER_SECOND,
                   "should converge in 10 seconds");

  sput_fail_unless(s->sent_multicast < 1000, "with 'few' multicast");

  sput_fail_unless(s->sent_unicast < 2000, "with 'few' unicast");

  /* Then, simulate network for a while, keeping eye on how often it's
   * NOT converged. */
  int converged_count = s->converged_count;
  int not_converged_count = s->not_converged_count;
#if L_LEVEL >= LOG_NOTICE
  int sent_unicast = s->sent_unicast;
#endif /* L_LEVEL >= LOG_NOTICE */
  hnetd_time_t convergence_time = hnetd_time();

  s->should_be_stable_topology = true;
  L_DEBUG("assume stable topology");
  SIM_WHILE(s, 100000, !net_sim_is_converged(s) ||
            (hnetd_time() - convergence_time) < (HNCP_INTERVAL_WORRIED * 2 *
                                                 HNCP_INTERVAL_RETRIES));
  L_NOTICE("unicasts sent:%d after convergence, last %lld ms after convergence",
           s->sent_unicast - sent_unicast, (long long)(s->last_unicast_sent - convergence_time));
#if 0
  /* As we do reachability checking, this isn't valid.. unfortunately. */
  sput_fail_unless((s->sent_unicast - sent_unicast) < 50,
                   "did not send (many) unicasts");
#endif /* 0 */
  sput_fail_unless(s->not_converged_count == not_converged_count,
                   "should stay converged");
  sput_fail_unless(s->converged_count > converged_count,
                   "converged count rising");

  L_DEBUG("assume unstable topology");
  s->should_be_stable_topology = false;

  /* Make sure it will converge after remove + re-add in reasonable
   * timeframe too. */
  net_sim_remove_node_by_name(s, nodenames[0]);

  /* Re-add the node */
  (void)net_sim_find_hncp(s, nodenames[0]);

  handle_connections(s, &nodeconnections[0], 2); /* Two first ones are needed */

  /* As the original node and the new node will wind up with exactly same
   * update #, but potentially wildly different timestamp on other nodes,
   * accept time errors in this case.
   * (Shouldn't accept in general case, however.)
   */

  s->accept_time_errors = true;

  SIM_WHILE(s, 10000, !net_sim_is_converged(s));

  net_sim_uninit(s);
}

void hncp_bird14()
{
  net_sim_s s;

  net_sim_init(&s);
  raw_bird14(&s);
}

void hncp_bird14_unique()
{
  net_sim_s s;

  net_sim_init(&s);
  s.use_global_iids = true;
  raw_bird14(&s);
}

bool no_conflicts = false;

static void raw_hncp_tube(net_sim s, unsigned int num_nodes)
{
  /* A LOT of routers connected in a tube (R1 R2 R3 .. RN). */
  unsigned int i;
  hncp_hash_s h1;
  hncp_hash_s h2;

  memset(&h1, 0, sizeof(h1));
  memset(&h2, 1, sizeof(h2));

  s->disable_sd = true;
  for (i = 0 ; i < num_nodes-1 ; i++)
    {
      char buf[128];

      sprintf(buf, "node%d", i);
      hncp n1 = net_sim_find_hncp(s, buf);
      /* Add intentional router ID collisions at nodes 0, 1,3 and 2 and 4 */
      if (!no_conflicts)
        {
          if (i == 0 || i == 1 || i == 3)
            hncp_set_own_hash(n1, &h1);
          else if (i == 2 || i == 4)
            hncp_set_own_hash(n1, &h2);
        }

      sprintf(buf, "node%d", i+1);
      hncp n2 = net_sim_find_hncp(s, buf);

      hncp_link l1 = net_sim_hncp_find_link_by_name(n1, "down");
      hncp_link l2 = net_sim_hncp_find_link_by_name(n2, "up");
      net_sim_set_connected(l1, l2, true);
      net_sim_set_connected(l2, l1, true);
    }
  SIM_WHILE(s, 100000, !net_sim_is_converged(s));

  sput_fail_unless(net_sim_find_hncp(s, "node0")->nodes.avl.count >= num_nodes,
                   "enough nodes");

  net_sim_uninit(s);
  L_NOTICE("finished in %lld ms", (long long)hnetd_time() - s->start);
}

void hncp_tube_small(void)
{
  net_sim_s s;

  net_sim_init(&s);
  raw_hncp_tube(&s, 6);
}

void hncp_tube_beyond_multicast(void)
{
  net_sim_s s;

  net_sim_init(&s);
  /* Intentionally pick a number that is >>
     IPv6 MTU / (HNCP_HASH_LEN * 2 + 2 * 4 + TLV_SIZE) =~ 28 */
  raw_hncp_tube(&s, 60);
}

void hncp_tube_beyond_multicast_unique(void)
{
  net_sim_s s;

  net_sim_init(&s);
  s.use_global_iids = true;
  /* Intentionally pick a number that is >>
     IPv6 MTU / (HNCP_HASH_LEN * 2 + 2 * 4 + TLV_SIZE) =~ 28 */
  raw_hncp_tube(&s, 60);
}

#define test_setup() srandom(seed)
#define maybe_run_test(fun) sput_maybe_run_test(fun, test_setup())

int main(__unused int argc, __unused char **argv)
{
#ifdef hnetd_time
#undef hnetd_time
#endif /* hnetd_time */
  int seed = (int)hnetd_time();
  int c;

  while ((c = getopt(argc, argv, "nr:")) > 0)
    {
      switch (c)
        {
        case 'r':
          seed = atoi(optarg);
          break;
        case 'n':
          no_conflicts = true;
          break;
        }
    }
  argc -= optind;
  argv += optind;

  setbuf(stdout, NULL); /* so that it's in sync with stderr when redirected */
  openlog("test_hncp_net", LOG_CONS | LOG_PERROR, LOG_DAEMON);

  L_INFO("starting with random seed %d", seed);
  sput_start_testing();
  sput_enter_suite("hncp_net"); /* optional */
  maybe_run_test(hncp_two);
  maybe_run_test(hncp_bird14);
  maybe_run_test(hncp_bird14_unique);
  maybe_run_test(hncp_tube_small);
  maybe_run_test(hncp_tube_beyond_multicast);
  maybe_run_test(hncp_tube_beyond_multicast_unique);
  sput_leave_suite(); /* optional */
  sput_finish_testing();
  return sput_get_return_value();
}
