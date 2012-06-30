/* Copyright (C) 2011-2012 B.A.T.M.A.N. contributors:
 *
 * Antonio Quartulli
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#include <linux/if_ether.h>
#include <linux/if_arp.h>

#include "main.h"
#include "distributed-arp-table.h"
#include "hard-interface.h"
#include "originator.h"
#include "send.h"
#include "types.h"
#include "unicast.h"

static void batadv_dat_purge(struct work_struct *work);

/**
 * batadv_dat_start_timer - initialise the DAT periodic worker
 * @bat_priv: the bat priv with all the soft interface information
 */
static void batadv_dat_start_timer(struct batadv_priv *bat_priv)
{
	INIT_DELAYED_WORK(&bat_priv->dat.work, batadv_dat_purge);
	queue_delayed_work(batadv_event_workqueue, &bat_priv->dat.work,
			   msecs_to_jiffies(10000));
}

/**
 * batadv_dat_entry_free_ref - decrements the dat_entry refcounter and possibly
 * free it
 * @dat_entry: the oentry to free
 */
static void batadv_dat_entry_free_ref(struct batadv_dat_entry *dat_entry)
{
	if (atomic_dec_and_test(&dat_entry->refcount))
		kfree_rcu(dat_entry, rcu);
}

/**
 * batadv_dat_to_purge - checks whether a dat_entry has to be purged or not
 * @dat_entry: the entry to check
 *
 * Returns true if the entry has to be purged now, false otherwise
 */
static bool batadv_dat_to_purge(struct batadv_dat_entry *dat_entry)
{
	return batadv_has_timed_out(dat_entry->last_update,
				    BATADV_DAT_ENTRY_TIMEOUT);
}

/**
 * __batadv_dat_purge - delete entries from the DAT local storage
 * @bat_priv: the bat priv with all the soft interface information
 * @to_purge: function in charge to decide whether an entry has to be purged or
 *	      not. This function takes the dat_entry as argument and has to
 *	      returns a boolean value: true is the entry has to be deleted,
 *	      false otherwise
 *
 * Loops over each entry in the DAT local storage and delete it if and only if
 * the to_purge function passed as argument returns true
 */
static void __batadv_dat_purge(struct batadv_priv *bat_priv,
			       bool (*to_purge)(struct batadv_dat_entry *))
{
	spinlock_t *list_lock; /* protects write access to the hash lists */
	struct batadv_dat_entry *dat_entry;
	struct hlist_node *node, *node_tmp;
	struct hlist_head *head;
	uint32_t i;

	if (!bat_priv->dat.hash)
		return;

	for (i = 0; i < bat_priv->dat.hash->size; i++) {
		head = &bat_priv->dat.hash->table[i];
		list_lock = &bat_priv->dat.hash->list_locks[i];

		spin_lock_bh(list_lock);
		hlist_for_each_entry_safe(dat_entry, node, node_tmp, head,
					  hash_entry) {
			/* if an helper function has been passed as parameter,
			 * ask it if the entry has to be purged or not
			 */
			if (to_purge && !to_purge(dat_entry))
				continue;

			hlist_del_rcu(node);
			batadv_dat_entry_free_ref(dat_entry);
		}
		spin_unlock_bh(list_lock);
	}
}

/**
 * batadv_dat_purge - periodic task that deletes old entries from the local DAT
 * hash table
 * @work: kernel work struct
 */
static void batadv_dat_purge(struct work_struct *work)
{
	struct delayed_work *delayed_work;
	struct batadv_priv_dat *priv_dat;
	struct batadv_priv *bat_priv;

	delayed_work = container_of(work, struct delayed_work, work);
	priv_dat = container_of(delayed_work, struct batadv_priv_dat, work);
	bat_priv = container_of(priv_dat, struct batadv_priv, dat);

	__batadv_dat_purge(bat_priv, batadv_dat_to_purge);
	batadv_dat_start_timer(bat_priv);
}

/**
 * batadv_compare_dat - comparing function used in the local DAT hash table
 * @node: node in the local table
 * @data2: second object to compare the node to
 *
 * Returns 1 if the two entry are the same, 0 otherwise
 */
static int batadv_compare_dat(const struct hlist_node *node, const void *data2)
{
	const void *data1 = container_of(node, struct batadv_dat_entry,
					 hash_entry);

	return (memcmp(data1, data2, sizeof(__be32)) == 0 ? 1 : 0);
}

/**
 * batadv_hash_dat - compute the hash value for an IP address
 * @data: data to hash
 * @size: size of the hash table
 *
 * Returns the selected index in the hash table for the given data
 */
static uint32_t batadv_hash_dat(const void *data, uint32_t size)
{
	const unsigned char *key = data;
	uint32_t hash = 0;
	size_t i;

	for (i = 0; i < 4; i++) {
		hash += key[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash % size;
}

/**
 * batadv_dat_entry_hash_find - looks for a given dat_entry in the local hash
 * table
 * @bat_priv: the bat priv with all the soft interface information
 * @ip: search key
 *
 * Returns the dat_entry if found, NULL otherwise
 */
static struct batadv_dat_entry *
batadv_dat_entry_hash_find(struct batadv_priv *bat_priv, __be32 ip)
{
	struct hlist_head *head;
	struct hlist_node *node;
	struct batadv_dat_entry *dat_entry, *dat_entry_tmp = NULL;
	struct batadv_hashtable *hash = bat_priv->dat.hash;
	uint32_t index;

	if (!hash)
		return NULL;

	index = batadv_hash_dat(&ip, hash->size);
	head = &hash->table[index];

	rcu_read_lock();
	hlist_for_each_entry_rcu(dat_entry, node, head, hash_entry) {
		if (dat_entry->ip != ip)
			continue;

		if (!atomic_inc_not_zero(&dat_entry->refcount))
			continue;

		dat_entry_tmp = dat_entry;
		break;
	}
	rcu_read_unlock();

	return dat_entry_tmp;
}

/**
 * batadv_dat_entry_add - add a new dat entry or update it if already exists
 * @bat_priv: the bat priv with all the soft interface information
 * @ip: ipv4 to add/edit
 * @mac_addr: mac address to assign to the given ipv4
 */
static void batadv_dat_entry_add(struct batadv_priv *bat_priv, __be32 ip,
				 uint8_t *mac_addr)
{
	struct batadv_dat_entry *dat_entry;
	int hash_added;

	dat_entry = batadv_dat_entry_hash_find(bat_priv, ip);
	/* if this entry is already known, just update it */
	if (dat_entry) {
		if (!batadv_compare_eth(dat_entry->mac_addr, mac_addr))
			memcpy(dat_entry->mac_addr, mac_addr, ETH_ALEN);
		dat_entry->last_update = jiffies;
		batadv_dbg(BATADV_DBG_DAT, bat_priv,
			   "Entry updated: %pI4 %pM\n", &dat_entry->ip,
			   dat_entry->mac_addr);
		goto out;
	}

	dat_entry = kmalloc(sizeof(*dat_entry), GFP_ATOMIC);
	if (!dat_entry)
		goto out;

	dat_entry->ip = ip;
	memcpy(dat_entry->mac_addr, mac_addr, ETH_ALEN);
	dat_entry->last_update = jiffies;
	atomic_set(&dat_entry->refcount, 2);

	hash_added = batadv_hash_add(bat_priv->dat.hash, batadv_compare_dat,
				     batadv_hash_dat, &dat_entry->ip,
				     &dat_entry->hash_entry);

	if (unlikely(hash_added != 0)) {
		/* remove the reference for the hash */
		batadv_dat_entry_free_ref(dat_entry);
		goto out;
	}

	batadv_dbg(BATADV_DBG_DAT, bat_priv, "New entry added: %pI4 %pM\n",
		   &dat_entry->ip, dat_entry->mac_addr);

out:
	if (dat_entry)
		batadv_dat_entry_free_ref(dat_entry);
}

/**
 * batadv_is_orig_node_eligible - check whether a node can be a DHT candidate
 * @res: the array with the already selected candidates
 * @select: number of already selected candidates
 * @tmp_max: address of the currently evaluated node
 * @max: current round max address
 * @last_max: address of the last selected candidate
 * @candidate: orig_node under evaluation
 * @max_orig_node: last selected candidate
 *
 * Returns true if the node has been elected as next candidate or false othrwise
 */
static bool batadv_is_orig_node_eligible(struct batadv_dat_candidate *res,
					 int select, batadv_dat_addr_t tmp_max,
					 batadv_dat_addr_t max,
					 batadv_dat_addr_t last_max,
					 struct batadv_orig_node *candidate,
					 struct batadv_orig_node *max_orig_node)
{
	bool ret = false;
	int j;

	/* Check if this node has already been selected... */
	for (j = 0; j < select; j++)
		if (res[j].orig_node == candidate)
			break;
	/* ..and possibly skip it */
	if (j < select)
		goto out;
	/* sanity check: has it already been selected? This should not happen */
	if (tmp_max > last_max)
		goto out;
	/* check if during this iteration an originator with a closer dht
	 * address has already been found
	 */
	if (tmp_max < max)
		goto out;
	/* this is an hash collision with the temporary selected node. Choose
	 * the one with the lowest address
	 */
	if ((tmp_max == max) &&
	    (batadv_compare_eth(candidate->orig, max_orig_node->orig) > 0))
		goto out;

	ret = true;
out:
	return ret;
}

/**
 * batadv_choose_next_candidate - select the next DHT candidate
 * @bat_priv: the bat priv with all the soft interface information
 * @cands: candidates array
 * @select: number of candidates already present in the array
 * @ip_key: key to look up in the DHT
 * @last_max: pointer where the address of the selected candidate will be saved
 */
static void batadv_choose_next_candidate(struct batadv_priv *bat_priv,
					 struct batadv_dat_candidate *cands,
					 int select, batadv_dat_addr_t ip_key,
					 batadv_dat_addr_t *last_max)
{
	batadv_dat_addr_t max = 0, tmp_max = 0;
	struct batadv_orig_node *orig_node, *max_orig_node = NULL;
	struct batadv_hashtable *hash = bat_priv->orig_hash;
	struct hlist_node *node;
	struct hlist_head *head;
	int i;

	/* if no node is eligible as candidate, leave the candidate type as
	 * NOT_FOUND
	 */
	cands[select].type = BATADV_DAT_CANDIDATE_NOT_FOUND;

	/* iterate over the originator list and find the node with closest
	 * dat_address which has not been selected yet
	 */
	for (i = 0; i < hash->size; i++) {
		head = &hash->table[i];

		rcu_read_lock();
		hlist_for_each_entry_rcu(orig_node, node, head, hash_entry) {
			/* the dht space is a ring and addresses are unsigned */
			tmp_max = BATADV_DAT_ADDR_MAX - orig_node->dat_addr +
				  ip_key;

			if (!batadv_is_orig_node_eligible(cands, select,
							  tmp_max, max,
							  *last_max, orig_node,
							  max_orig_node))
				continue;

			if (!atomic_inc_not_zero(&orig_node->refcount))
				continue;

			max = tmp_max;
			if (max_orig_node)
				batadv_orig_node_free_ref(max_orig_node);
			max_orig_node = orig_node;
		}
		rcu_read_unlock();
	}
	if (max_orig_node) {
		cands[select].type = BATADV_DAT_CANDIDATE_ORIG;
		cands[select].orig_node = max_orig_node;
		batadv_dbg(BATADV_DBG_DAT, bat_priv,
			   "dat_select_candidates() %d: selected %pM addr=%u dist=%u\n",
			   select, max_orig_node->orig, max_orig_node->dat_addr,
			   max);
	}
	*last_max = max;
}

/**
 * batadv_dat_select_candidates - selects the nodes which the DHT message has to
 * be sent to
 * @bat_priv: the bat priv with all the soft interface information
 * @ip_dst: ipv4 to look up in the DHT
 *
 * An originator O is selected if and only if its DHT_ID value is one of three
 * closest values (from the LEFT, with wrap around if needed) then the hash
 * value of the key. ip_dst is the key.
 *
 * Returns the candidate array of size BATADV_DAT_CANDIDATE_NUM
 */
static struct batadv_dat_candidate *
batadv_dat_select_candidates(struct batadv_priv *bat_priv, __be32 ip_dst)
{
	int select;
	batadv_dat_addr_t last_max = BATADV_DAT_ADDR_MAX, ip_key;
	struct batadv_dat_candidate *res;

	if (!bat_priv->orig_hash)
		return NULL;

	res = kmalloc(BATADV_DAT_CANDIDATES_NUM * sizeof(*res), GFP_ATOMIC);
	if (!res)
		return NULL;

	ip_key = (batadv_dat_addr_t)batadv_hash_dat(&ip_dst,
						    BATADV_DAT_ADDR_MAX);

	batadv_dbg(BATADV_DBG_DAT, bat_priv,
		   "dat_select_candidates(): IP=%pI4 hash(IP)=%u\n", &ip_dst,
		   ip_key);

	for (select = 0; select < BATADV_DAT_CANDIDATES_NUM; select++)
		batadv_choose_next_candidate(bat_priv, res, select, ip_key,
					     &last_max);

	return res;
}

/**
 * batadv_dat_send_data - send a payload to the selected candidates
 * @bat_priv: the bat priv with all the soft interface information
 * @skb: payload to send
 * @ip: the DHT key
 * @packet_subtype: unicast4addr packet subtype to use
 *
 * In this function the skb is copied by means of pskb_copy() and is sent as
 * unicast packet to each of the selected candidates
 *
 * Returns true if the packet is sent to at least one candidate, false otherwise
 */
static bool batadv_dat_send_data(struct batadv_priv *bat_priv,
				 struct sk_buff *skb, __be32 ip,
				 int packet_subtype)
{
	int i;
	bool ret = false;
	int send_status;
	struct batadv_neigh_node *neigh_node = NULL;
	struct sk_buff *tmp_skb;
	struct batadv_dat_candidate *cand;

	cand = batadv_dat_select_candidates(bat_priv, ip);
	if (!cand)
		goto out;

	batadv_dbg(BATADV_DBG_DAT, bat_priv, "DHT_SEND for %pI4\n", &ip);

	for (i = 0; i < BATADV_DAT_CANDIDATES_NUM; i++) {
		if (cand[i].type == BATADV_DAT_CANDIDATE_NOT_FOUND)
			continue;

		neigh_node = batadv_orig_node_get_router(cand[i].orig_node);
		if (!neigh_node)
			goto free_orig;

		tmp_skb = pskb_copy(skb, GFP_ATOMIC);
		if (!batadv_unicast_4addr_prepare_skb(bat_priv, tmp_skb,
						      cand[i].orig_node,
						      packet_subtype)) {
			kfree_skb(tmp_skb);
			goto free_neigh;
		}

		send_status = batadv_send_skb_packet(tmp_skb,
						     neigh_node->if_incoming,
						     neigh_node->addr);
		if (send_status == NET_XMIT_SUCCESS)
			/* packet sent to a candidate: return true */
			ret = true;
free_neigh:
		batadv_neigh_node_free_ref(neigh_node);
free_orig:
		batadv_orig_node_free_ref(cand[i].orig_node);
	}

out:
	kfree(cand);
	return ret;
}

/**
 * batadv_dat_hash_free - free the local DAT hash table
 * @bat_priv: the bat priv with all the soft interface information
 */
static void batadv_dat_hash_free(struct batadv_priv *bat_priv)
{
	__batadv_dat_purge(bat_priv, NULL);

	batadv_hash_destroy(bat_priv->dat.hash);

	bat_priv->dat.hash = NULL;
}

/**
 * batadv_dat_init - initialise the DAT internals
 * @bat_priv: the bat priv with all the soft interface information
 */
int batadv_dat_init(struct batadv_priv *bat_priv)
{
	if (bat_priv->dat.hash)
		return 0;

	bat_priv->dat.hash = batadv_hash_new(1024);

	if (!bat_priv->dat.hash)
		return -ENOMEM;

	batadv_dat_start_timer(bat_priv);

	return 0;
}

/**
 * batadv_dat_free - free the DAT internals
 * @bat_priv: the bat priv with all the soft interface information
 */
void batadv_dat_free(struct batadv_priv *bat_priv)
{
	cancel_delayed_work_sync(&bat_priv->dat.work);

	batadv_dat_hash_free(bat_priv);
}

/**
 * batadv_dat_cache_seq_print_text - print the local DAT hash table
 * @seq: seq file to print on
 * @offset: not used
 */
int batadv_dat_cache_seq_print_text(struct seq_file *seq, void *offset)
{
	struct net_device *net_dev = (struct net_device *)seq->private;
	struct batadv_priv *bat_priv = netdev_priv(net_dev);
	struct batadv_hashtable *hash = bat_priv->dat.hash;
	struct batadv_dat_entry *dat_entry;
	struct batadv_hard_iface *primary_if;
	struct hlist_node *node;
	struct hlist_head *head;
	unsigned long last_seen_jiffies;
	int last_seen_msecs, last_seen_secs, last_seen_mins;
	uint32_t i;

	primary_if = batadv_seq_print_text_primary_if_get(seq);
	if (!primary_if)
		goto out;

	seq_printf(seq, "Distributed ARP Table (%s):\n", net_dev->name);
	seq_printf(seq, "          %-7s          %-13s %5s\n", "IPv4", "MAC",
		   "last-seen");

	for (i = 0; i < hash->size; i++) {
		head = &hash->table[i];

		rcu_read_lock();
		hlist_for_each_entry_rcu(dat_entry, node, head, hash_entry) {
			last_seen_jiffies = jiffies - dat_entry->last_update;
			last_seen_msecs = jiffies_to_msecs(last_seen_jiffies);
			last_seen_mins = last_seen_msecs / 60000;
			last_seen_msecs = last_seen_msecs % 60000;
			last_seen_secs = last_seen_msecs / 1000;

			seq_printf(seq, " * %15pI4 %14pM %6i:%02i\n",
				   &dat_entry->ip, dat_entry->mac_addr,
				   last_seen_mins, last_seen_secs);
		}
		rcu_read_unlock();
	}

out:
	if (primary_if)
		batadv_hardif_free_ref(primary_if);
	return 0;
}