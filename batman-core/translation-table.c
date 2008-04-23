/*
 * Copyright (C) 2007-2008 B.A.T.M.A.N. contributors:
 * Marek Lindner
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
 *
 */





#include "main.h"
#include "translation-table.h"
#include "log.h"
#include "soft-interface.h"
#include "types.h"
#include "hash.h"



struct hashtable_t *hna_local_hash = NULL;
static struct hashtable_t *hna_global_hash = NULL;

DEFINE_SPINLOCK(hna_local_hash_lock);
DEFINE_SPINLOCK(hna_global_hash_lock);

static struct timer_list hna_local_timer;



static void hna_local_start_timer(void)
{
	init_timer(&hna_local_timer);

	hna_local_timer.expires = jiffies + (10 * HZ); /* 10 seconds */
	hna_local_timer.data = 0;
	hna_local_timer.function = hna_local_purge;

	add_timer(&hna_local_timer);
}

int hna_local_init(void)
{
	hna_local_hash = hash_new(128, compare_orig, choose_orig);

	if (hna_local_hash == NULL)
		return 0;

	hna_local_start_timer();

	return 1;
}

void hna_local_add(uint8_t *addr)
{
	struct hna_local_entry *hna_local_entry;
	struct hna_global_entry *hna_global_entry;
	struct hashtable_t *swaphash;
	char hna_str[ETH_STR_LEN];

	spin_lock(&hna_local_hash_lock);
	hna_local_entry = ((struct hna_local_entry *)hash_find(hna_local_hash, addr));
	spin_unlock(&hna_local_hash_lock);

	if (hna_local_entry != NULL) {
		hna_local_entry->last_seen = jiffies;
		return;
	}

	addr_to_string(hna_str, addr);

	/* only announce as many hosts as possible in the batman-packet and space in batman_packet->num_hna
	   That also should give a limit to MAC-flooding. */
	if ((num_hna + 1 > (1500 - sizeof(struct batman_packet)) / 6) || (num_hna + 1 > 255)) {
		debug_log(LOG_TYPE_ROUTES, "Can't add new local hna entry (%s): number of local hna entries exceeds packet size \n", hna_str);
		return;
	}

	debug_log(LOG_TYPE_ROUTES, "Creating new local hna entry: %s \n", hna_str);

	hna_local_entry = kmalloc(sizeof(struct hna_local_entry), GFP_KERNEL);

	if (!hna_local_entry)
		return;

	memcpy(hna_local_entry->addr, addr, ETH_ALEN);
	hna_local_entry->last_seen = jiffies;

	/* the batman interface mac address should never be purged */
	if (compare_orig(addr, bat_device->dev_addr))
		hna_local_entry->never_purge = 1;
	else
		hna_local_entry->never_purge = 0;

	spin_lock(&hna_local_hash_lock);

	hash_add(hna_local_hash, hna_local_entry);
	num_hna++;
	hna_local_changed = 1;

	if (hna_local_hash->elements * 4 > hna_local_hash->size) {
		swaphash = hash_resize(hna_local_hash, hna_local_hash->size * 2);

		if (swaphash == NULL)
			debug_log(LOG_TYPE_CRIT, "Couldn't resize local hna hash table \n");
		else
			hna_local_hash = swaphash;
	}

	spin_unlock(&hna_local_hash_lock);

	/* remove address from global hash if present */
	spin_lock(&hna_global_hash_lock);

	hna_global_entry = ((struct hna_global_entry *)hash_find(hna_global_hash, addr));

	if (hna_global_entry != NULL)
		_hna_global_del_orig(hna_global_entry, "local hna received");

	spin_unlock(&hna_global_hash_lock);
}

int hna_local_fill_buffer(unsigned char *buff, int buff_len)
{
	struct hna_local_entry *hna_local_entry;
	struct hash_it_t *hashit = NULL;
	int i = 0;

	spin_lock(&hna_local_hash_lock);

	while (NULL != (hashit = hash_iterate(hna_local_hash, hashit))) {

		if (buff_len < (i + 1) * ETH_ALEN)
			break;

		hna_local_entry = hashit->bucket->data;
		memcpy(buff + (i * ETH_ALEN), hna_local_entry->addr, ETH_ALEN);

		i++;
	}

	/* if we did not get all new local hnas see you next time  ;-) */
	if (i == num_hna)
		hna_local_changed = 0;

	spin_unlock(&hna_local_hash_lock);

	return i;
}

int hna_local_fill_buffer_text(unsigned char *buff, int buff_len)
{
	struct hna_local_entry *hna_local_entry;
	struct hash_it_t *hashit = NULL;
	int bytes_written = 0;

	spin_lock(&hna_local_hash_lock);

	while (NULL != (hashit = hash_iterate(hna_local_hash, hashit))) {

		if (buff_len < bytes_written + ETH_STR_LEN + 4)
			break;

		hna_local_entry = hashit->bucket->data;

		bytes_written += snprintf(buff + bytes_written, ETH_STR_LEN + 4, " * %02x:%02x:%02x:%02x:%02x:%02x\n",
					hna_local_entry->addr[0], hna_local_entry->addr[1], hna_local_entry->addr[2],
					hna_local_entry->addr[3], hna_local_entry->addr[4], hna_local_entry->addr[5]);
	}

	spin_unlock(&hna_local_hash_lock);

	return bytes_written;
}

static void _hna_local_del(void *data)
{
	kfree(data);
	num_hna--;
	hna_local_changed = 1;
}

static void hna_local_del(struct hna_local_entry *hna_local_entry, char *message)
{
	char hna_str[ETH_STR_LEN];

	addr_to_string(hna_str, hna_local_entry->addr);
	debug_log(LOG_TYPE_ROUTES, "Deleting local hna entry (%s): %s \n", hna_str, message);

	hash_remove(hna_local_hash, hna_local_entry->addr);
	_hna_local_del(hna_local_entry);
}

void hna_local_purge(unsigned long data)
{
	struct hna_local_entry *hna_local_entry;
	struct hash_it_t *hashit = NULL;

	spin_lock(&hna_local_hash_lock);

	while (NULL != (hashit = hash_iterate(hna_local_hash, hashit))) {

		hna_local_entry = hashit->bucket->data;

		if ((!hna_local_entry->never_purge) && (time_after(jiffies, hna_local_entry->last_seen + (LOCAL_HNA_TIMEOUT /1000) * HZ)))
			hna_local_del(hna_local_entry, "address timeouted");
	}

	spin_unlock(&hna_local_hash_lock);
	hna_local_start_timer();
}

void hna_local_free(void)
{
	if (hna_local_hash != NULL) {
		del_timer_sync(&hna_local_timer);
		hash_delete(hna_local_hash, _hna_local_del);
	}
}

int hna_global_init(void)
{
	hna_global_hash = hash_new(128, compare_orig, choose_orig);

	if (hna_global_hash == NULL)
		return 0;

	return 1;
}

void hna_global_add_orig(struct orig_node *orig_node, unsigned char *hna_buff, int hna_buff_len)
{
	struct hna_global_entry *hna_global_entry;
	struct hna_local_entry *hna_local_entry;
	struct hashtable_t *swaphash;
	char hna_str[ETH_STR_LEN], orig_str[ETH_STR_LEN];
	int hna_buff_count = 0;

	addr_to_string(orig_str, orig_node->orig);

	while ((hna_buff_count + 1) * ETH_ALEN <= hna_buff_len) {

		spin_lock(&hna_global_hash_lock);

		hna_global_entry = ((struct hna_global_entry *)hash_find(hna_global_hash, hna_buff + (hna_buff_count * ETH_ALEN)));

		if (hna_global_entry == NULL) {

			spin_unlock(&hna_global_hash_lock);

			hna_global_entry = kmalloc(sizeof(struct hna_global_entry), GFP_KERNEL);

			if (!hna_global_entry)
				break;

			memcpy(hna_global_entry->addr, hna_buff + (hna_buff_count * ETH_ALEN), ETH_ALEN);

			addr_to_string(hna_str, hna_global_entry->addr);
			debug_log(LOG_TYPE_ROUTES, "Creating new global hna entry: %s (via %s)\n", hna_str, orig_str);

			spin_lock(&hna_global_hash_lock);
			hash_add(hna_global_hash, hna_global_entry);

		}

		hna_global_entry->orig_node = orig_node;
		spin_unlock(&hna_global_hash_lock);

		/* remove address from local hash if present */
		spin_lock(&hna_local_hash_lock);

		hna_local_entry = ((struct hna_local_entry *)hash_find(hna_local_hash, hna_buff + (hna_buff_count * ETH_ALEN)));

		if (hna_local_entry != NULL)
			hna_local_del(hna_local_entry, "global hna received");

		spin_unlock(&hna_local_hash_lock);

		hna_buff_count++;
	}

	orig_node->hna_buff_len = hna_buff_len;

	if (orig_node->hna_buff_len > 0) {
		orig_node->hna_buff = kmalloc(orig_node->hna_buff_len, GFP_KERNEL);
		memcpy(orig_node->hna_buff, hna_buff, orig_node->hna_buff_len);
	} else {
		orig_node->hna_buff = NULL;
	}

	spin_lock(&hna_global_hash_lock);

	if (hna_global_hash->elements * 4 > hna_global_hash->size) {
		swaphash = hash_resize(hna_global_hash, hna_global_hash->size * 2);

		if (swaphash == NULL)
			debug_log(LOG_TYPE_CRIT, "Couldn't resize global hna hash table \n");
		else
			hna_global_hash = swaphash;
	}

	spin_unlock(&hna_global_hash_lock);
}

int hna_global_fill_buffer_text(unsigned char *buff, int buff_len)
{
	struct hna_global_entry *hna_global_entry;
	struct hash_it_t *hashit = NULL;
	int bytes_written = 0;

	spin_lock(&hna_global_hash_lock);

	while (NULL != (hashit = hash_iterate(hna_global_hash, hashit))) {

		if (buff_len < bytes_written + (2 * ETH_STR_LEN) + 10)
			break;

		hna_global_entry = hashit->bucket->data;

		bytes_written += snprintf(buff + bytes_written, (2 * ETH_STR_LEN) + 10,
					" * %02x:%02x:%02x:%02x:%02x:%02x via %02x:%02x:%02x:%02x:%02x:%02x \n",
					hna_global_entry->addr[0], hna_global_entry->addr[1], hna_global_entry->addr[2],
					hna_global_entry->addr[3], hna_global_entry->addr[4], hna_global_entry->addr[5],
					hna_global_entry->orig_node->orig[0], hna_global_entry->orig_node->orig[1],
					hna_global_entry->orig_node->orig[2], hna_global_entry->orig_node->orig[3],
					hna_global_entry->orig_node->orig[4], hna_global_entry->orig_node->orig[5]);
	}

	spin_unlock(&hna_global_hash_lock);

	return bytes_written;
}

void _hna_global_del_orig(struct hna_global_entry *hna_global_entry, char *message)
{
	char hna_str[ETH_STR_LEN], orig_str[ETH_STR_LEN];

	addr_to_string(orig_str, hna_global_entry->orig_node->orig);
	addr_to_string(hna_str, hna_global_entry->addr);

	debug_log(LOG_TYPE_ROUTES, "Deleting global hna entry %s (via %s): %s \n", hna_str, orig_str, message);

	hash_remove(hna_global_hash, hna_global_entry->addr);
	kfree(hna_global_entry);
}

void hna_global_del_orig(struct orig_node *orig_node, char *message)
{
	struct hna_global_entry *hna_global_entry;
	int hna_buff_count = 0;

	if (orig_node->hna_buff_len == 0)
		return;

	spin_lock(&hna_global_hash_lock);

	while ((hna_buff_count + 1) * ETH_ALEN <= orig_node->hna_buff_len) {

		hna_global_entry = ((struct hna_global_entry *)hash_find(hna_global_hash, orig_node->hna_buff + (hna_buff_count * ETH_ALEN)));

		if ((hna_global_entry != NULL) && (hna_global_entry->orig_node == orig_node))
			_hna_global_del_orig(hna_global_entry, message);

		hna_buff_count++;
	}

	spin_unlock(&hna_global_hash_lock);

	orig_node->hna_buff_len = 0;
	kfree(orig_node->hna_buff);
	orig_node->hna_buff = NULL;
}

static void hna_global_del(void *data)
{
	kfree(data);
}

void hna_global_free(void)
{
	if (hna_global_hash != NULL)
		hash_delete(hna_global_hash, hna_global_del);
}

struct orig_node *transtable_search(uint8_t *addr)
{
	struct hna_global_entry *hna_global_entry;

	spin_lock(&hna_global_hash_lock);
	hna_global_entry = ((struct hna_global_entry *)hash_find(hna_global_hash, addr));
	spin_unlock(&hna_global_hash_lock);

	if (hna_global_entry == NULL)
		return NULL;

	return hna_global_entry->orig_node;
}

