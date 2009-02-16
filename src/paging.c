/* Paging helper and manager.... */
/* (C) 2009 by Holger Hans Peter Freyther <zecke@selfish.org>
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/*
 * Relevant specs:
 *     12.21:
 *       - 9.4.12 for CCCH Local Threshold
 *
 *     05.58:
 *       - 8.5.2 CCCH Load indication
 *       - 9.3.15 Paging Load
 *
 * Approach:
 *       - Send paging command to subscriber
 *       - On Channel Request we will remember the reason
 *       - After the ACK we will request the identity
 *	 - Then we will send assign the gsm_subscriber and
 *	 - and call a callback
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <openbsc/paging.h>
#include <openbsc/debug.h>
#include <openbsc/signal.h>
#include <openbsc/abis_rsl.h>
#include <openbsc/gsm_data.h>

#define PAGING_TIMEOUT 1, 75000
#define MAX_PAGING_REQUEST 750

static unsigned int calculate_group(struct gsm_bts *bts, struct gsm_subscriber *subscr)
{
	int ccch_conf;
	int bs_cc_chans;
	int blocks;
	unsigned int group;
	
	ccch_conf = bts->chan_desc.ccch_conf;
	bs_cc_chans = rsl_ccch_conf_to_bs_cc_chans(ccch_conf);
	/* code word + 2, as 2 channels equals 0x0 */
	blocks = rsl_number_of_paging_subchannels(bts);
	group = get_paging_group(str_to_imsi(subscr->imsi),
					bs_cc_chans, blocks);
	return group;
}

/*
 * Kill one paging request update the internal list...
 */
static void paging_remove_request(struct gsm_bts_paging_state *paging_bts,
				struct gsm_paging_request *to_be_deleted)
{
	/* Update the last_request if that is necessary */
	if (to_be_deleted == paging_bts->last_request) {
		paging_bts->last_request =
			(struct gsm_paging_request *)paging_bts->last_request->entry.next;
		if (&to_be_deleted->entry == &paging_bts->pending_requests)
			paging_bts->last_request = NULL;
	}

	del_timer(&to_be_deleted->T3113);
	llist_del(&to_be_deleted->entry);
	subscr_put(to_be_deleted->subscr);
	free(to_be_deleted);
}

static void page_ms(struct gsm_paging_request *request)
{
	u_int8_t mi[128];
	unsigned long int tmsi;
	unsigned int mi_len;
	unsigned int page_group;

	DEBUGP(DPAG, "Going to send paging commands: '%s'\n",
		request->subscr->imsi);

	page_group = calculate_group(request->bts, request->subscr);
	tmsi = strtoul(request->subscr->tmsi, NULL, 10);
	mi_len = generate_mid_from_tmsi(mi, tmsi);
	rsl_paging_cmd(request->bts, page_group, mi_len, mi,
			request->chan_type);
}

static void paging_move_to_next(struct gsm_bts_paging_state *paging_bts)
{
	paging_bts->last_request =
		(struct gsm_paging_request *)paging_bts->last_request->entry.next;
	if (&paging_bts->last_request->entry == &paging_bts->pending_requests)
		paging_bts->last_request = NULL;
}

/*
 * This is kicked by the periodic PAGING LOAD Indicator
 * coming from abis_rsl.c
 *
 * We attempt to iterate once over the list of items but
 * only upto available_slots.
 */
static void paging_handle_pending_requests(struct gsm_bts_paging_state *paging_bts)
{
	struct gsm_paging_request *initial_request = NULL;
	struct gsm_paging_request *current_request = NULL;

	/*
	 * Determine if the pending_requests list is empty and
	 * return then.
	 */
	if (&paging_bts->pending_requests == paging_bts->pending_requests.next) {
		paging_bts->last_request = NULL;
		return;
	}

	if (!paging_bts->last_request)
		paging_bts->last_request =
			(struct gsm_paging_request *)paging_bts->pending_requests.next;

	assert(paging_bts->last_request);
	initial_request = paging_bts->last_request;
	current_request = initial_request;

	do {
		/* handle the paging request now */
		page_ms(current_request);
		paging_bts->available_slots--;

		/*
		 * move to the next item. We might wrap around
		 * this means last_request will be NULL and we just
		 * call paging_page_to_next again. It it guranteed
		 * that the list is not empty.
		 */
		paging_move_to_next(paging_bts);
		if (!paging_bts->last_request)
			paging_bts->last_request =
				(struct gsm_paging_request *)paging_bts->pending_requests.next;
		current_request = paging_bts->last_request;
	} while (paging_bts->available_slots > 0
		    &&  initial_request != current_request);
}

void paging_init(struct gsm_bts *bts)
{
	bts->paging.bts = bts;
	INIT_LLIST_HEAD(&bts->paging.pending_requests);

	/* Large number, until we get a proper message */
	bts->paging.available_slots = 10;
}

static int paging_pending_request(struct gsm_bts_paging_state *bts,
				struct gsm_subscriber *subscr) {
	struct gsm_paging_request *req;

	llist_for_each_entry(req, &bts->pending_requests, entry) {
		if (subscr == req->subscr)
			return 1;
	}

	return 0;	
}

static void paging_T3113_expired(void *data)
{
	struct gsm_paging_request *req = (struct gsm_paging_request *)data;
	struct paging_signal_data sig_data;

	DEBUGP(DPAG, "T3113 expired for request %p (%s)\n",
		req, req->subscr->imsi);
	
	sig_data.subscr = req->subscr,
	sig_data.bts	= req->bts,
	sig_data.lchan	= NULL,

	dispatch_signal(SS_PAGING, S_PAGING_COMPLETED, &sig_data);
	paging_remove_request(&req->bts->paging, req);
}

void paging_request(struct gsm_bts *bts, struct gsm_subscriber *subscr, int type) {
	struct gsm_bts_paging_state *bts_entry = &bts->paging;
	struct gsm_paging_request *req;

	if (paging_pending_request(bts_entry, subscr)) {
		DEBUGP(DPAG, "Paging request already pending\n");
		return;
	}

	req = (struct gsm_paging_request *)malloc(sizeof(*req));
	memset(req, 0, sizeof(*req));
	req->subscr = subscr_get(subscr);
	req->bts = bts;
	req->chan_type = type;
	req->T3113.cb = paging_T3113_expired;
	req->T3113.data = req;
	schedule_timer(&req->T3113, T3113_VALUE);
	llist_add_tail(&req->entry, &bts_entry->pending_requests);
}

/* we consciously ignore the type of the request here */
void paging_request_stop(struct gsm_bts *bts, struct gsm_subscriber *subscr)
{
	struct gsm_bts_paging_state *bts_entry = &bts->paging;
	struct gsm_paging_request *req, *req2;

	llist_for_each_entry_safe(req, req2, &bts_entry->pending_requests,
				 entry) {
		if (req->subscr == subscr) {
			paging_remove_request(&bts->paging, req);
			break;
		}
	}
}

void paging_update_buffer_space(struct gsm_bts *bts, u_int16_t free_slots)
{
	bts->paging.available_slots = free_slots;
	paging_handle_pending_requests(&bts->paging);
}
