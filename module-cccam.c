#define MODULE_LOG_PREFIX "cccam"

#include "globals.h"

#ifdef MODULE_CCCAM

#include "cscrypt/md5.h"
#include "cscrypt/sha1.h"
#include "module-cacheex.h"
#include "module-cccam.h"
#include "module-cccam-data.h"
#include "module-cccam-cacheex.h"
#include "module-cccshare.h"
#include "oscam-chk.h"
#include "oscam-cache.h"
#include "oscam-client.h"
#include "oscam-ecm.h"
#include "oscam-emm.h"
#include "oscam-failban.h"
#include "oscam-garbage.h"
#include "oscam-lock.h"
#include "oscam-net.h"
#include "oscam-reader.h"
#include "oscam-string.h"
#include "oscam-time.h"
#include "oscam-work.h"

// Mode names for CMD_05 command
static const char *cmd05_mode_name[] = { "UNKNOWN", "PLAIN", "AES", "CC_CRYPT", "RC4", "LEN=0" };

// Mode names for CMD_0C command
static const char *cmd0c_mode_name[] = { "NONE", "RC6", "RC4", "CC_CRYPT", "AES", "IDEA" };

const char *cc_msg_name[]={"MSG_CLI_DATA","MSG_CW_ECM","MSG_EMM_ACK","MSG_VALUE_03",
			    "MSG_CARD_REMOVED","MSG_CMD_05","MSG_KEEPALIVE","MSG_NEW_CARD",
			    "MSG_SRV_DATA","MSG_VALUE_09","MSG_NEW_CARD_SIDINFO","MSG_CW_NOK1",
			    "MSG_CW_NOK2","MSG_NO_HEADER"};

char * cc_get_msgname(uint32_t msg,char *result,uint32_t len){
	if(msg <= 0x09)
		return (char*)cc_msg_name[msg];
	else if(msg == 0x0f)
		return (char*)cc_msg_name[10];
	else if(msg == 0xfe)
		return (char*)cc_msg_name[11];
	else if(msg == 0xff)
		return (char*)cc_msg_name[12];
	else if(msg == 0xffff)
		return (char*)cc_msg_name[13];
	else if(msg>=0x0a && msg<=0x0e){
		snprintf(result,len,"MSG_CMD_%02x",msg);
		return result;
	}else{
		snprintf(result,len,"MSG_VALUE_%02x",msg);
		return result;
	}
}
uint8_t cc_node_id[8];

int32_t cc_cli_connect(struct s_client *cl);
int32_t cc_send_pending_emms(struct s_client *cl);

#define getprefix() (!cl ? "" : (!cl->cc ? "" : (((struct cc_data *)(cl->cc))->prefix)))

void cc_init_crypt(struct cc_crypt_block *block, uint8_t *key, int32_t len)
{
	int32_t i = 0;
	uint8_t j = 0;

	for(i = 0; i < 256; i++)
	{
		block->keytable[i] = i;
	}

	for(i = 0; i < 256; i++)
	{
		j += key[i % len] + block->keytable[i];
		SWAPC(&block->keytable[i], &block->keytable[j]);
	}

	block->state = *key;
	block->counter = 0;
	block->sum = 0;
}

void cc_crypt(struct cc_crypt_block *block, uint8_t *data, int32_t len, cc_crypt_mode_t mode)
{
	int32_t i;
	uint8_t z;

	for(i = 0; i < len; i++)
	{
		block->counter++;
		block->sum += block->keytable[block->counter];
		SWAPC(&block->keytable[block->counter], &block->keytable[block->sum]);

		z = data[i];
		data[i] = z ^ block->keytable[(block->keytable[block->counter] + block->keytable[block->sum]) & 0xff];
		data[i] ^= block->state;

		if(!mode)
		{
			z = data[i];
		}

		block->state = block->state ^ z;
	}
}

void cc_rc4_crypt(struct cc_crypt_block *block, uint8_t *data, int32_t len, cc_crypt_mode_t mode)
{
	int32_t i;
	uint8_t z;

	for(i = 0; i < len; i++)
	{
		block->counter++;
		block->sum += block->keytable[block->counter];
		SWAPC(&block->keytable[block->counter], &block->keytable[block->sum]);

		z = data[i];
		data[i] = z ^ block->keytable[(block->keytable[block->counter] + block->keytable[block->sum]) & 0xff];

		if(!mode)
		{
			z = data[i];
		}

		block->state = block->state ^ z;
	}
}

void cc_xor(uint8_t *buf)
{
	const char cccam[] = "CCcam";
	uint8_t i;

	for(i = 0; i < 8; i++)
	{
		buf[8 + i] = i * buf[i];

		if(i <= 5)
		{
			buf[i] ^= cccam[i];
		}
	}
}

void cc_cw_crypt(struct s_client *cl, uint8_t *cws, uint32_t cardid)
{
	struct cc_data *cc = cl->cc;
	uint8_t n = 0;
	uint8_t i;
	uint8_t tmp;
	uint8_t *nod = NULL;

	if (!cs_malloc(&nod, 8))
	{
		return;
	}

	for (i=0; i<8; i++)
	{
		if (cl->typ != 'c')
		{
			nod[i] = cc->node_id[7-i];
		}
		else
		{
			nod[i] = cc->peer_node_id[7-i];
		}
	}

	for (i=0; i<16; i++)
	{
		if (i & 1)
		{
			if (i != 15)
			{
				n = (nod[i>>1]>>4) | (nod[(i>>1)+1]<<4);
			}
			else
			{
				n = nod[i>>1]>>4;
			}
		}
		else
		{
			n = nod[i>>1];
		}

		n = n & 0xff;
		tmp = cws[i] ^ n;

		if (i & 1)
		{
			tmp = ~tmp;
		}

		cws[i] = ((cardid >> (2 * i)) ^ tmp) & 0xff;
	}

	free(nod);
}

/** swap endianness (int) */
static void SwapLBi(uint8_t *buff, int32_t len)
{
#if __BYTE_ORDER != __BIG_ENDIAN
	return;
#endif

	int32_t i;
	uint8_t swap[4];

	for(i = 0; i < len / 4; i++)
	{
		memcpy(swap, buff, 4);
		buff[0] = swap[3];
		buff[1] = swap[2];
		buff[2] = swap[1];
		buff[3] = swap[0];
		buff += 4;
	}
}

void cc_crypt_cmd0c(struct s_client *cl, uint8_t *buf, int32_t len)
{
	struct cc_data *cc = cl->cc;
	uint8_t *out;

	if(!cs_malloc(&out, len))
	{
		return;
	}

	switch(cc->cmd0c_mode)
	{
		case MODE_CMD_0x0C_NONE: // none additional encryption
		{
			memcpy(out, buf, len);
			break;
		}

		case MODE_CMD_0x0C_RC6: // RC6
		{
			// buf may be unaligned,
			// so we use malloc() memory for the uint32_t* cast
			uint8_t *tmp;
			int32_t i;

			if(!cs_malloc(&tmp, len))
			{
				return;
			}

			memcpy(tmp, buf, len);
			SwapLBi(tmp, len);

			for(i = 0; i < len / 16; i++)
			{
				rc6_block_decrypt((uint32_t *)(tmp + i * 16), (uint32_t *)(out + i * 16), 1, cc->cmd0c_RC6_cryptkey);
			}

			SwapLBi(out, len);
			NULLFREE(tmp);
			break;
		}

		case MODE_CMD_0x0C_RC4: // RC4
		{
			cc_rc4_crypt(&cc->cmd0c_cryptkey, buf, len, ENCRYPT);
			memcpy(out, buf, len);
			break;
		}

		case MODE_CMD_0x0C_CC_CRYPT: // cc_crypt
		{
			cc_crypt(&cc->cmd0c_cryptkey, buf, len, DECRYPT);
			memcpy(out, buf, len);
			break;
		}

		case MODE_CMD_0x0C_AES: // AES
		{
			int32_t i;
			for(i = 0; i < len / 16; i++)
			{
				AES_decrypt((uint8_t *)buf + i * 16, (uint8_t *)out + i * 16, &cc->cmd0c_AES_key);
			}
			break;
		}

		case MODE_CMD_0x0C_IDEA: // IDEA
		{
			int32_t i = 0;
			int32_t j;

			while(i < len)
			{
				idea_ecb_encrypt(buf + i, out + i, &cc->cmd0c_IDEA_dkey);
				i += 8;
			}

			i = 8;
			while(i < len)
			{
				for(j = 0; j < 8; j++)
				{
					out[j + i] ^= buf[j + i - 8];
				}
				i += 8;
			}

			break;
		}
	}
	memcpy(buf, out, len);
	NULLFREE(out);
}

void set_cmd0c_cryptkey(struct s_client *cl, uint8_t *key, uint8_t len)
{
	struct cc_data *cc = cl->cc;
	uint8_t key_buf[32];

	memset(&key_buf, 0, sizeof(key_buf));

	if(len > 32)
	{
		len = 32;
	}

	memcpy(key_buf, key, len);

	switch(cc->cmd0c_mode)
	{
		case MODE_CMD_0x0C_NONE: // NONE
		{
			break;
		}

		case MODE_CMD_0x0C_RC6: // RC6
		{
			rc6_key_setup(key_buf, 32, cc->cmd0c_RC6_cryptkey);
			break;
		}

		case MODE_CMD_0x0C_RC4: // RC4
		case MODE_CMD_0x0C_CC_CRYPT: // CC_CRYPT
		{
			cc_init_crypt(&cc->cmd0c_cryptkey, key_buf, 32);
			break;
		}

		case MODE_CMD_0x0C_AES: // AES
		{
			memset(&cc->cmd0c_AES_key, 0, sizeof(cc->cmd0c_AES_key));
			AES_set_decrypt_key((uint8_t *)key_buf, 256, &cc->cmd0c_AES_key);
			break;
		}

		case MODE_CMD_0x0C_IDEA: // IDEA
		{
			uint8_t key_buf_idea[16];
			memcpy(key_buf_idea, key_buf, 16);
			IDEA_KEY_SCHEDULE ekey;

			idea_set_encrypt_key(key_buf_idea, &ekey);
			idea_set_decrypt_key(&ekey, &cc->cmd0c_IDEA_dkey);
			break;
		}
	}
}

int32_t sid_eq(struct cc_srvid *srvid1, struct cc_srvid *srvid2)
{
	return (srvid1->sid == srvid2->sid && (srvid1->chid == srvid2->chid || !srvid1->chid || !srvid2->chid)
				&& (srvid1->ecmlen == srvid2->ecmlen || !srvid1->ecmlen || !srvid2->ecmlen));
}

int32_t sid_eq_nb(struct cc_srvid *srvid1, struct cc_srvid_block *srvid2)
{
	return sid_eq(srvid1, (struct cc_srvid *)srvid2);
}

int32_t sid_eq_bb(struct cc_srvid_block *srvid1, struct cc_srvid_block *srvid2)
{
	return (srvid1->sid == srvid2->sid && (srvid1->chid == srvid2->chid || !srvid1->chid || !srvid2->chid)
				&& (srvid1->ecmlen == srvid2->ecmlen || !srvid1->ecmlen || !srvid2->ecmlen)
				&& (srvid1->blocked_till == srvid2->blocked_till || !srvid1->blocked_till || !srvid2->blocked_till));
}

struct cc_srvid_block *is_sid_blocked(struct cc_card *card, struct cc_srvid *srvid_blocked)
{
	LL_ITER it = ll_iter_create(card->badsids);
	struct cc_srvid_block *srvid;

	while((srvid = ll_iter_next(&it)))
	{
		if(sid_eq_nb(srvid_blocked, srvid))
		{
			break;
		}
		else if(srvid->ecmlen && ((struct cc_srvid_block *)srvid)->blocked_till > time(NULL))
		{
			ll_iter_remove_data(&it);
		}		
	}
	return srvid;
}

uint32_t has_perm_blocked_sid(struct cc_card *card)
{
	LL_ITER it = ll_iter_create(card->badsids);
	struct cc_srvid_block *srvid;

	while((srvid = ll_iter_next(&it)))
	{
		if(srvid->blocked_till == 0)
		{
			break;
		}
	}
	return srvid != NULL;
}

struct cc_srvid *is_good_sid(struct cc_card *card, struct cc_srvid *srvid_good)
{
	LL_ITER it = ll_iter_create(card->goodsids);
	struct cc_srvid *srvid;

	while((srvid = ll_iter_next(&it)))
	{
		if(sid_eq(srvid, srvid_good))
		{
			break;
		}
	}
	return srvid;
}

#define BLOCKING_SECONDS 6

void add_sid_block(struct cc_card *card, struct cc_srvid *srvid_blocked, bool temporary)
{
	if(is_sid_blocked(card, srvid_blocked))
	{
		return;
	}

	struct cc_srvid_block *srvid;
	if(!cs_malloc(&srvid, sizeof(struct cc_srvid_block)))
	{
		return;
	}
	memcpy(srvid, srvid_blocked, sizeof(struct cc_srvid));

	if(temporary)
	{
		srvid->blocked_till = time(NULL) + BLOCKING_SECONDS;
	}

	ll_append(card->badsids, srvid);
	cs_log_dbg(D_READER, "added sid block %04X(CHID %04X, length %d) for card %08x",
				srvid_blocked->sid, srvid_blocked->chid, srvid_blocked->ecmlen, card->id);
}

void remove_sid_block(struct cc_card *card, struct cc_srvid *srvid_blocked)
{
	LL_ITER it = ll_iter_create(card->badsids);
	struct cc_srvid_block *srvid;

	while((srvid = ll_iter_next(&it)))
	{
		if(sid_eq_nb(srvid_blocked, srvid))
		{
			ll_iter_remove_data(&it);
		}
	}

	cs_log_dbg(D_READER, "removed sid block %04X(CHID %04X, length %d) for card %08x",
				srvid_blocked->sid, srvid_blocked->chid, srvid_blocked->ecmlen, card->id);
}

void add_good_sid(struct cc_card *card, struct cc_srvid *srvid_good)
{
	if(is_good_sid(card, srvid_good))
	{
		return;
	}

	remove_sid_block(card, srvid_good);
	struct cc_srvid *srvid;

	if(!cs_malloc(&srvid, sizeof(struct cc_srvid)))
	{
		return;
	}

	memcpy(srvid, srvid_good, sizeof(struct cc_srvid));
	ll_append(card->goodsids, srvid);

	cs_log_dbg(D_READER, "added good sid %04X(%d) for card %08x",
				srvid_good->sid, srvid_good->ecmlen, card->id);
}

void remove_good_sid(struct cc_card *card, struct cc_srvid *srvid_good)
{
	LL_ITER it = ll_iter_create(card->goodsids);
	struct cc_srvid *srvid;

	while((srvid = ll_iter_next(&it)))
	{
		if(sid_eq(srvid, srvid_good))
		{
			ll_iter_remove_data(&it);
		}
	}

	cs_log_dbg(D_READER, "removed good sid %04X(%d) for card %08x",
				srvid_good->sid, srvid_good->ecmlen, card->id);
}

/**
 * reader
 * clears and frees values for reinit
 */
void cc_cli_close(struct s_client *cl, int32_t call_conclose)
{
	struct s_reader *rdr = cl->reader;
	struct cc_data *cc = cl->cc;

	if(!rdr || !cc)
	{
		return;
	}

	if(rdr)
	{
		rdr->tcp_connected = 0;
	}

	if(rdr)
	{
		rdr->card_status = NO_CARD;
	}

	if(rdr)
	{
		rdr->last_s = rdr->last_g = 0;
	}

	if(cl)
	{
		cl->last = 0;
	}

	if(call_conclose) // clears also pending ecms!
	{
		network_tcp_connection_close(rdr, "close");
	}
	else
	{
		if(cl->udp_fd)
		{
			close(cl->udp_fd);
			cl->udp_fd = 0;
			cl->pfd = 0;
		}
	}

	cc->ecm_busy = 0;
	cc->just_logged_in = 0;
}

struct cc_extended_ecm_idx *add_extended_ecm_idx(struct s_client *cl, uint8_t send_idx, uint16_t ecm_idx,
												struct cc_card *card, struct cc_srvid srvid, int8_t free_card)
{
	struct cc_data *cc = cl->cc;
	struct cc_extended_ecm_idx *eei;

	if(!cs_malloc(&eei, sizeof(struct cc_extended_ecm_idx)))
	{
		return NULL;
	}

	eei->send_idx = send_idx;
	eei->ecm_idx = ecm_idx;
	eei->card = card;
	eei->cccam_id = card->id;
	eei->srvid = srvid;
	eei->free_card = free_card;
	cs_ftime(&eei->tps);
	ll_append(cc->extended_ecm_idx, eei);
	//cs_log_dbg(D_TRACE, "%s add extended ecm-idx: %d:%d", getprefix(), send_idx, ecm_idx);
	return eei;
}

struct cc_extended_ecm_idx *get_extended_ecm_idx(struct s_client *cl, uint8_t send_idx, int32_t remove_item)
{
	struct cc_data *cc = cl->cc;
	struct cc_extended_ecm_idx *eei;
	LL_ITER it = ll_iter_create(cc->extended_ecm_idx);

	while((eei = ll_iter_next(&it)))
	{
		if(eei->send_idx == send_idx)
		{
			if(remove_item)
			{
				ll_iter_remove(&it);
			}

			//cs_log_dbg(D_TRACE, "%s get by send-idx: %d FOUND: %d", getprefix(), send_idx, eei->ecm_idx);
			return eei;
		}
	}

#ifdef WITH_DEBUG
	if(remove_item)
	{
		cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s get by send-idx: %d NOT FOUND", getprefix(), send_idx);
	}
#endif

	return NULL;
}

struct cc_extended_ecm_idx *get_extended_ecm_idx_by_idx(struct s_client *cl, uint16_t ecm_idx, int32_t remove_item)
{
	struct cc_data *cc = cl->cc;
	struct cc_extended_ecm_idx *eei;
	LL_ITER it = ll_iter_create(cc->extended_ecm_idx);

	while((eei = ll_iter_next(&it)))
	{
		if(eei->ecm_idx == ecm_idx)
		{
			if(remove_item)
			{
				ll_iter_remove(&it);
			}

			//cs_log_dbg(D_TRACE, "%s get by ecm-idx: %d FOUND: %d", getprefix(), ecm_idx, eei->send_idx);
			return eei;
		}
	}

#ifdef WITH_DEBUG
	if(remove_item)
	{
		cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s get by ecm-idx: %d NOT FOUND", getprefix(), ecm_idx);
	}
#endif

	return NULL;
}

void cc_reset_pending(struct s_client *cl, int32_t ecm_idx)
{
	int32_t i = 0;

	for(i = 0; i < cfg.max_pending; i++)
	{
		if(cl->ecmtask[i].idx == ecm_idx && cl->ecmtask[i].rc == E_ALREADY_SENT)
		{
			cl->ecmtask[i].rc = E_UNHANDLED; // Mark unused
		}
	}
}

void free_extended_ecm_idx_by_card(struct s_client *cl, struct cc_card *card, int8_t null_only)
{
	struct cc_data *cc = cl->cc;
	struct cc_extended_ecm_idx *eei;
	LL_ITER it = ll_iter_create(cc->extended_ecm_idx);

	while((eei = ll_iter_next(&it)))
	{
		if(eei->card == card)
		{
			if(null_only)
			{
				cc_reset_pending(cl, eei->ecm_idx);
				if(eei->free_card)
				{
					NULLFREE(eei->card);
				}
				ll_iter_remove_data(&it);
			}
			else
			{
				if(eei->free_card)
				{
					NULLFREE(eei->card);
				}
				eei->card = NULL;
			}
		}
	}
}

void free_extended_ecm_idx(struct cc_data *cc)
{
	struct cc_extended_ecm_idx *eei;
	LL_ITER it = ll_iter_create(cc->extended_ecm_idx);

	while((eei = ll_iter_next(&it)))
	{
		if(eei->free_card)
		{
			NULLFREE(eei->card);
		}
		ll_iter_remove_data(&it);
	}
}

int32_t cc_recv_to(struct s_client *cl, uint8_t *buf, int32_t len)
{
	int32_t rc;
	struct pollfd pfd;

	while(1)
	{
		pfd.fd = cl->udp_fd;
		pfd.events = POLLIN | POLLPRI;

		rc = poll(&pfd, 1, cfg.cc_recv_timeout);

		if(rc < 0)
		{
			if(errno == EINTR)
			{
				continue;
			}

			return -1; // error!!
		}

		if(rc == 1)
		{
			if(pfd.revents & POLLHUP)
			{
				return -1; // hangup = error!!
			}
			else
			{
				break;
			}
		}
		else
		{
			return -2; // timeout!!
		}
	}
	return cs_recv(cl->udp_fd, buf, len, MSG_WAITALL);
}

/**
 * reader
 * closes the connection and reopens it.
 */
static int8_t cc_cycle_connection(struct s_client *cl)
{
	if(!cl || cl->kill)
	{
		return 0;
	}

	cs_log_dbg(D_TRACE, "%s unlocked-cycleconnection! timeout %d ms", getprefix(), cl->reader->cc_reconnect);

	cc_cli_close(cl, 0);
	cs_sleepms(50);
	cc_cli_connect(cl);

	return cl->reader->tcp_connected;
}

/**
 * reader + server:
 * receive a message
 */
int32_t cc_msg_recv(struct s_client *cl, uint8_t *buf, int32_t maxlen)
{
	struct s_reader *rdr = (cl->typ == 'c') ? NULL : cl->reader;

	int32_t len;
	struct cc_data *cc = cl->cc;
	int32_t handle = cl->udp_fd;

	if(handle <= 0 || maxlen < 4)
	{
		return -1;
	}

	if(!cl->cc)
	{
		return -1;
	}

	cs_writelock(__func__, &cc->lockcmd);

	if(!cl->cc)
	{
		cs_writeunlock(__func__, &cc->lockcmd);
		return -1;
	}

	len = cs_recv(handle, buf, 4, MSG_WAITALL);

	if(len != 4) // invalid header length read
	{
		if(len <= 0)
		{
			cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s disconnected by remote server", getprefix());
		}
		else
		{
			cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s invalid header length (expected 4, read %d)",
						getprefix(), len);
		}

		cs_writeunlock(__func__, &cc->lockcmd);
		return -1;
	}

	cc_crypt(&cc->block[DECRYPT], buf, 4, DECRYPT);
	//cs_log_dump_dbg(D_CLIENT, buf, 4, "cccam: decrypted header:");

	cc->g_flag = buf[0];

	int32_t size = (buf[2] << 8) | buf[3];
	if(size) // check if any data is expected in msg
	{
		if(size > maxlen)
		{
			cs_writeunlock(__func__, &cc->lockcmd);
			cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s message too big (size=%d max=%d)",
						getprefix(), size, maxlen);
			return 0;
		}

		len = cs_recv(handle, buf + 4, size, MSG_WAITALL);

		if(rdr && (buf[1] == MSG_CW_ECM
#ifdef CS_CACHEEX_AIO
			 || buf[1] == MSG_CW_ECM_LGF
#endif
		))
		{
			rdr->last_g = time(NULL);
		}

		if(len != size)
		{
			cs_writeunlock(__func__, &cc->lockcmd);

			if(len <= 0)
			{
				cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s disconnected by remote", getprefix());
			}
			else
			{
				cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s invalid message length read (expected %d, read %d)",
							getprefix(), size, len);
			}
			return -1;
		}

		cc_crypt(&cc->block[DECRYPT], buf + 4, len, DECRYPT);
		len += 4;
	}

	cs_writeunlock(__func__, &cc->lockcmd);

	//cs_log_dump_dbg(cl->typ=='c'?D_CLIENT:D_READER, buf, len, "cccam: full decrypted msg, len=%d:", len);

	return len;
}

/**
 * reader + server
 * send a message
 */
int32_t cc_cmd_send(struct s_client *cl, uint8_t *buf, int32_t len, cc_msg_type_t cmd)
{
	if(!cl->udp_fd) // disconnected
	{
		return -1;
	}

	struct s_reader *rdr = (cl->typ == 'c') ? NULL : cl->reader;

	int32_t n;
	struct cc_data *cc = cl->cc;

	if(!cl->cc || cl->kill)
	{
		return -1;
	}

	cs_writelock(__func__, &cc->lockcmd);

	if(!cl->cc || cl->kill)
	{
		cs_writeunlock(__func__, &cc->lockcmd);
		return -1;
	}

	uint8_t *netbuf;
	if(!cs_malloc(&netbuf, len + 4))
	{
		cs_writeunlock(__func__, &cc->lockcmd);
		return -1;
	}

	if(cmd == MSG_NO_HEADER)
	{
		memcpy(netbuf, buf, len);
	}
	else
	{
		// build command message
		netbuf[0] = cc->g_flag; // flags?
		netbuf[1] = cmd & 0xff;
		netbuf[2] = len >> 8;
		netbuf[3] = len & 0xff;

		if(buf)
		{
			memcpy(netbuf + 4, buf, len);
		}
		len += 4;
	}

	cs_log_dump_dbg(D_CLIENT, netbuf, len, "cccam: send:");
	cc_crypt(&cc->block[ENCRYPT], netbuf, len, ENCRYPT);

	n = send(cl->udp_fd, netbuf, len, 0);

	cs_writeunlock(__func__, &cc->lockcmd);

	NULLFREE(netbuf);

	if(n != len)
	{
		if(rdr)
		{
			cc_cli_close(cl, 1);
		}
		else
		{
			cs_disconnect_client(cl);
		}
		n = -1;
	}

	return n;
}

#define CC_DEFAULT_VERSION 9
#define CC_VERSIONS 10
static char *version[CC_VERSIONS]  = { "2.0.11", "2.1.1", "2.1.2", "2.1.3", "2.1.4", "2.2.0", "2.2.1", "2.3.0", "2.3.1", "2.3.2"};
static char *build[CC_VERSIONS]    = { "2892",   "2971",  "3094",  "3165",  "3191",  "3290",  "3316",  "3367",  "9d508a",  "4000"};
static char extcompat[CC_VERSIONS] = { 0,        0,       0,       0,       0,       1,       1,       1,       1,       1}; // Supporting new card format starting with 2.2.0

/**
 * reader + server
 * checks the cccam-version in the configuration
 */
void cc_check_version(char *cc_version, char *cc_build)
{
	int32_t i;
	for(i = 0; i < CC_VERSIONS; i++)
	{
		if(!memcmp(cc_version, version[i], cs_strlen(version[i])))
		{
			memcpy(cc_build, build[i], cs_strlen(build[i]) + 1);
			cs_log_dbg(D_CLIENT, "cccam: auto build set for version: %s build: %s", cc_version, cc_build);
			return;
		}
	}

	memcpy(cc_version, version[CC_DEFAULT_VERSION], cs_strlen(version[CC_DEFAULT_VERSION]));
	memcpy(cc_build, build[CC_DEFAULT_VERSION], cs_strlen(build[CC_DEFAULT_VERSION]));

	cs_log_dbg(D_CLIENT, "cccam: auto version set: %s build: %s", cc_version, cc_build);

	return;
}

int32_t check_cccam_compat(struct cc_data *cc)
{
	int32_t res = 0;
	int32_t i = 0;
	for(i = 0; i < CC_VERSIONS; i++)
	{
		if(!strcmp(cfg.cc_version, version[i]))
		{
			res += extcompat[i];
			break;
		}
	}

	if(!res)
	{
		return 0;
	}

	for(i = 0; i < CC_VERSIONS; i++)
	{
		if(!strcmp(cc->remote_version, version[i]))
		{
			res += extcompat[i];
			break;
		}
	}

	return res == 2;
}

/**
 * reader
 * sends own version information to the CCCam server
 */
int32_t cc_send_cli_data(struct s_client *cl)
{
	struct s_reader *rdr = cl->reader;
	struct cc_data *cc = cl->cc;
	const int32_t size = 20 + 8 + 6 + 26 + 4 + 28 + 1;
	uint8_t buf[size];

	cs_log_dbg(D_READER, "cccam: send client data");

	memcpy(cc->node_id, cc_node_id, sizeof(cc_node_id));

	memcpy(buf, rdr->r_usr, sizeof(rdr->r_usr));
	memcpy(buf + 20, cc->node_id, 8);
	buf[28] = 0; // <-- Client wants to have EMUs, 0 - NO; 1 - YES
	memcpy(buf + 29, rdr->cc_version, sizeof(rdr->cc_version)); // cccam version (ascii)
	memcpy(buf + 61, rdr->cc_build, sizeof(rdr->cc_build)); // build number (ascii)

	// multics seed already detected, now send multics 'WHO' for getting and confirming multics server
	if(cc->multics_mode == 1)
	{
		memcpy(buf + 57, "W", 1);
		memcpy(buf + 58, "H", 1);
		memcpy(buf + 59, "O", 1);
	}

	cs_log_dbg(D_READER, "%s sending own version: %s, build: %s", getprefix(), rdr->cc_version, rdr->cc_build);

	return cc_cmd_send(cl, buf, size, MSG_CLI_DATA);
}

/**
 * server
 * sends version information to the client
 */
int32_t cc_send_srv_data(struct s_client *cl)
{
	struct cc_data *cc = cl->cc;

	cs_log_dbg(D_CLIENT, "cccam: send server data");

	memcpy(cc->node_id, cc_node_id, sizeof(cc_node_id));

	uint8_t buf[0x48];
	memset(buf, 0, 0x48);

	int32_t stealth = cl->account->cccstealth;
	if(stealth == -1)
	{
		stealth = cfg.cc_stealth;
	}

	if(stealth)
	{
		cc->node_id[7]++;
	}

	memcpy(buf, cc->node_id, 8);

	char cc_build[7], tmp_dbg[17];
	memset(cc_build, 0, sizeof(cc_build));
	cc_check_version((char *) cfg.cc_version, cc_build);
	memcpy(buf + 8, cfg.cc_version, sizeof(cfg.cc_version)); // cccam version (ascii)
	memcpy(buf + 40, cc_build, sizeof(cc_build)); // build number (ascii)

	cs_log_dbg(D_CLIENT, "%s version: %s, build: %s nodeid: %s", getprefix(),
				cfg.cc_version, cc_build, cs_hexdump(0, cc->peer_node_id, 8, tmp_dbg, sizeof(tmp_dbg)));

	return cc_cmd_send(cl, buf, 0x48, MSG_SRV_DATA);
}

int32_t loop_check(uint8_t *myid, struct s_client *cl)
{
	if(!cl || !myid)
	{
		return 0;
	}

	struct cc_data *cc = cl->cc;
	if(!cc)
	{
		return 0;
	}

	return !memcmp(myid, cc->peer_node_id, sizeof(cc->peer_node_id)); // same nodeid? ignore
}

/**
 * reader
 * retrieves the next waiting ecm request
 */
int32_t cc_get_nxt_ecm(struct s_client *cl)
{
	struct cc_data *cc = cl->cc;
	ECM_REQUEST *er, *ern = NULL;
	int32_t n, i, pending = 0;
	struct timeb t;

	cs_ftime(&t);
	int32_t diff = (int32_t)cfg.ctimeout + 500;

	n = -1;
	for(i = 0; i < cfg.max_pending; i++)
	{
		er = &cl->ecmtask[i];
		if((comp_timeb(&t, &er->tps) >= diff) && (er->rc >= E_NOCARD)) // drop timeouts
		{
			write_ecm_answer(cl->reader, er, E_TIMEOUT, 0, NULL, NULL, 0, NULL);
		}

		else if(er->rc >= E_NOCARD && er->rc <= E_UNHANDLED) // stil active and waiting
		{
			pending++;
			if(loop_check(cc->peer_node_id, er->client))
			{
				cs_log_dbg(D_READER, "%s ecm loop detected! client %s (%8lX)",
							getprefix(), er->client->account->usr, (unsigned long)er->client->thread);
				write_ecm_answer(cl->reader, er, E_NOTFOUND, E2_CCCAM_LOOP, NULL, NULL, 0, NULL);
			}
			else
			{
				// search for the ecm with the lowest time, this should be the next to go
				if(n < 0 || (ern->tps.time - er->tps.time < 0))
				{
					// check for already pending:
					if(cc && cc->extended_mode)
					{
						int32_t j, found;
						ECM_REQUEST *erx;

						for(found = j = 0; j < cfg.max_pending; j++)
						{
							erx = &cl->ecmtask[j];
							if(i != j && erx->rc == E_ALREADY_SENT
								&& er->caid == erx->caid && er->ecmd5 == erx->ecmd5)
							{
								found = 1;
								break;
							}
						}

						if(!found)
						{
							n = i;
							ern = er;
						}
					}
					else
					{
						n = i;
						ern = er;
					}
				}
			}
		}
	}

	cl->pending = pending;
	return n;
}

/**
 * sends the secret cmd05 answer to the server
 */
int32_t send_cmd05_answer(struct s_client *cl)
{
	struct cc_data *cc = cl->cc;
	if(!cc->cmd05_active || cc->ecm_busy) // exit if not in cmd05 or waiting for ECM answer
	{
		return 0;
	}

	cc->cmd05_active--;
	if(cc->cmd05_active)
	{
		return 0;
	}

	uint8_t *data = cc->cmd05_data;
	cc_cmd05_mode cmd05_mode = MODE_UNKNOWN;

	// by Project: Keynation
	switch(cc->cmd05_data_len)
	{
		case 0: // payload 0, return with payload 0!
		{
			cc_cmd_send(cl, NULL, 0, MSG_CMD_05);
			cmd05_mode = MODE_LEN0;
			break;
		}

		case 256:
		{
			cmd05_mode = cc->cmd05_mode;
			switch(cmd05_mode)
			{
				case MODE_PLAIN: // send plain unencrypted back
				{
					cc_cmd_send(cl, data, 256, MSG_CMD_05);
					break;
				}

				case MODE_AES: // encrypt with received aes128 key
				{
					AES_KEY key;
					uint8_t aeskey[16];
					uint8_t out[256];

					memcpy(aeskey, cc->cmd05_aeskey, 16);
					memset(&key, 0, sizeof(key));

					AES_set_encrypt_key((uint8_t *) &aeskey, 128, &key);
					int32_t i;

					for(i = 0; i < 256; i += 16)
					{
						AES_encrypt((uint8_t *)data + i, (uint8_t *) &out + i, &key);
					}

					cc_cmd_send(cl, out, 256, MSG_CMD_05);
					break;
				}

				case MODE_CC_CRYPT: // encrypt with cc_crypt
				{
					cc_crypt(&cc->cmd05_cryptkey, data, 256, ENCRYPT);
					cc_cmd_send(cl, data, 256, MSG_CMD_05);
					break;
				}

				case MODE_RC4_CRYPT: // special xor crypt
				{
					cc_rc4_crypt(&cc->cmd05_cryptkey, data, 256, DECRYPT);
					cc_cmd_send(cl, data, 256, MSG_CMD_05);
					break;
				}

				default:
					cmd05_mode = MODE_UNKNOWN;
			}
			break;
		}

		default:
			cmd05_mode = MODE_UNKNOWN;
	}

	// unhandled types always needs cycle connection after 50 ECMs!
	if(cmd05_mode == MODE_UNKNOWN)
	{
		cc_cmd_send(cl, NULL, 0, MSG_CMD_05);
		if(!cc->max_ecms) // max_ecms already set?
		{
			cc->max_ecms = 50;
			cc->ecm_counter = 0;
		}
	}

	cs_log_dbg(D_READER, "%s sending CMD_05 back! MODE: %s len=%d",
				getprefix(), cmd05_mode_name[cmd05_mode], cc->cmd05_data_len);

	cc->cmd05NOK = 1;
	return 1;
}

int32_t get_UA_ofs(uint16_t caid)
{
	int32_t ofs = 0;
	switch(caid >> 8)
	{
		case 0x05: // VIACCESS
		case 0x0D: // CRYPTOWORKS
			//ofs = 1;
			//break;
		case 0x4A: // TONGFANG
		case 0x09: // VIDEOGUARD
		case 0x0B: // CONAX
		case 0x18: // NAGRA
		case 0x01: // SECA
		case 0x00: // SECAMANAGMENT
		case 0x17: // BETACRYPT
		case 0x06: // IRDETO
			ofs = 2;
			break;
	}

	return ofs;
}

int32_t UA_len(uint8_t *ua)
{
	int32_t i, len = 0;

	for(i = 0; i < 8; i++)
	{
		if(ua[i])
		{
			len++;
		}
	}
	return len;
}
void UA_left(uint8_t *in, uint8_t *out, int32_t ofs)
{
	memset(out, 0, 8);
	memcpy(out, in + ofs, 8 - ofs);
}

void UA_right(uint8_t *in, uint8_t *out, int32_t len)
{
	int32_t ofs = 0;

	while(len)
	{
		memcpy(out + ofs, in, len);
		len--;

		if(out[len])
		{
			break;
		}
		ofs++;
		out[0] = 0;
	}
}

/**
 * cccam uses UA right justified
 **/
void cc_UA_oscam2cccam(uint8_t *in, uint8_t *out, uint16_t caid)
{
	uint8_t tmp[8];
	memset(out, 0, 8);
	memset(tmp, 0, 8);

	//switch(caid >> 8)
	//{
	//	case 0x17: //IRDETO/Betacrypt:
	//		//oscam: AA BB CC DD 00 00 00 00
	//		//cccam: 00 00 00 00 DD AA BB CC
	//		out[4] = in[3]; //Hexbase
	//		out[5] = in[0];
	//		out[6] = in[1];
	//		out[7] = in[2];
	//		return;
	//
	//		//Place here your own adjustments!
	//}

	if(caid_is_bulcrypt(caid))
	{
		out[4] = in[0];
		out[5] = in[1];
		out[6] = in[2];
		out[7] = in[3];
		return;
	}

	hexserial_to_newcamd(in, tmp + 2, caid);
	UA_right(tmp, out, 8);
}

/**
 * oscam has a special format, depends on offset or type:
 **/
void cc_UA_cccam2oscam(uint8_t *in, uint8_t *out, uint16_t caid)
{
	uint8_t tmp[8];
	memset(out, 0, 8);
	memset(tmp, 0, 8);

	//switch(caid >> 8)
	//{
	//	case 0x17: //IRDETO/Betacrypt:
	//		//cccam: 00 00 00 00 DD AA BB CC
	//		//oscam: AA BB CC DD 00 00 00 00
	//		out[0] = in[5];
	//		out[1] = in[6];
	//		out[2] = in[7];
	//		out[3] = in[4]; //Hexbase
	//		return;

	//		//Place here your own adjustments!
	//}

	if(caid_is_bulcrypt(caid))
	{
		out[0] = in[4];
		out[1] = in[5];
		out[2] = in[6];
		out[3] = in[7];
		return;
	}

	int32_t ofs = get_UA_ofs(caid);
	UA_left(in, tmp, ofs);
	newcamd_to_hexserial(tmp, out, caid);
}

void cc_SA_oscam2cccam(uint8_t *in, uint8_t *out)
{
	memcpy(out, in, 4);
}

void cc_SA_cccam2oscam(uint8_t *in, uint8_t *out)
{
	memcpy(out, in, 4);
}

int32_t cc_UA_valid(uint8_t *ua)
{
	int32_t i;

	for(i = 0; i < 8; i++)
	{
		if(ua[i])
		{
			return 1;
		}
	}

	return 0;
}

/**
 * Updates AU Data: UA (Unique ID / Hexserial) und SA (Shared ID - Provider)
 */
void set_au_data(struct s_client *cl, struct s_reader *rdr, struct cc_card *card, ECM_REQUEST *cur_er)
{
	if(rdr->audisabled || !cc_UA_valid(card->hexserial))
	{
		return;
	}

	struct cc_data *cc = cl->cc;
	char tmp_dbg[17];
	cc->last_emm_card = card;

	cc_UA_cccam2oscam(card->hexserial, rdr->hexserial, rdr->caid);

	cs_log_dbg(D_EMM, "%s au info: caid %04X UA: %s", getprefix(), card->caid,
				cs_hexdump(0, rdr->hexserial, 8, tmp_dbg, sizeof(tmp_dbg)));

	rdr->nprov = 0;
	LL_ITER it2 = ll_iter_create(card->providers);
	struct cc_provider *provider;
	int32_t p = 0;

	while((provider = ll_iter_next(&it2)))
	{
		if(!cur_er || provider->prov == cur_er->prid || !provider->prov || !cur_er->prid)
		{
			rdr->prid[p][0] = provider->prov >> 24;
			rdr->prid[p][1] = provider->prov >> 16;
			rdr->prid[p][2] = provider->prov >> 8;
			rdr->prid[p][3] = provider->prov & 0xFF;
			cc_SA_cccam2oscam(provider->sa, rdr->sa[p]);

			cs_log_dbg(D_EMM, "%s au info: provider: %06X:%02X%02X%02X%02X", getprefix(),
						provider->prov, provider->sa[0], provider->sa[1], provider->sa[2], provider->sa[3]);

			p++;
			rdr->nprov = p;
			if(p >= CS_MAXPROV)
			{
				break;
			}
		}
	}

	if(!rdr->nprov) // No Providers? Add null-provider
	{
		memset(rdr->prid[0], 0, sizeof(rdr->prid[0]));
		rdr->nprov = 1;
	}

	rdr->caid = card->caid;
	if(cur_er)
	{
		rdr->auprovid = cur_er->prid;
	}
}

int32_t same_first_node(struct cc_card *card1, struct cc_card *card2)
{
	uint8_t *node1 = ll_has_elements(card1->remote_nodes);
	uint8_t *node2 = ll_has_elements(card2->remote_nodes);

	if(!node1 && !node2)
	{
		return 1;  // both NULL, same!
	}

	if(!node1 || !node2)
	{
		return 0; // one NULL, not same!
	}

	return !memcmp(node1, node2, 8); // same?
}

int32_t same_card2(struct cc_card *card1, struct cc_card *card2, int8_t compare_grp)
{
	return (card1->caid == card2->caid &&
			card1->card_type == card2->card_type &&
			card1->sidtab == card2->sidtab &&
			(!compare_grp || card1->grp == card2->grp) &&
			!memcmp(card1->hexserial, card2->hexserial, sizeof(card1->hexserial)));
}

int32_t same_card(struct cc_card *card1, struct cc_card *card2)
{
	return (card1->remote_id == card2->remote_id &&
			same_card2(card1, card2, 1) &&
			same_first_node(card1, card2));
}

struct cc_card *get_matching_card(struct s_client *cl, ECM_REQUEST *cur_er, int8_t chk_only)
{
	struct cc_data *cc = cl->cc;
	struct s_reader *rdr = cl->reader;

	if(cl->kill || !rdr || !cc)
	{
		return NULL;
	}

	struct cc_srvid cur_srvid;
	cur_srvid.sid = cur_er->srvid;
	cur_srvid.chid = cur_er->chid;
	cur_srvid.ecmlen = cur_er->ecmlen;

	int32_t best_rating = MIN_RATING - 1, rating;

	LL_ITER it = ll_iter_create(cc->cards);
	struct cc_card *card = NULL, *ncard, *xcard = NULL;

	while((ncard = ll_iter_next(&it)))
	{
		int lb_match = 0;
		if(config_enabled(WITH_LB))
		{
			// accept beta card when beta-tunnel is on
			lb_match = chk_only && cfg.lb_mode && cfg.lb_auto_betatunnel &&
					((caid_is_nagra(cur_er->caid) && caid_is_betacrypt(ncard->caid) && cfg.lb_auto_betatunnel_mode <= 3) ||
					(caid_is_betacrypt(cur_er->caid) && caid_is_nagra(ncard->caid) && cfg.lb_auto_betatunnel_mode >= 1));
		}

		if((ncard->caid == cur_er->caid // caid matches
			|| lb_match)) // or system matches if caid ends with 00
		{
			int32_t goodSidCount = ll_count(ncard->goodsids);
			int32_t badSidCount = ll_count(ncard->badsids);
			struct cc_srvid *good_sid;
			struct cc_srvid_block *blocked_sid;

			if(goodSidCount && !badSidCount) // only good sids -> check if sid is good
			{
				good_sid = is_good_sid(ncard, &cur_srvid);
				if(!good_sid)
				{
					continue;
				}
			}
			else if(!goodSidCount && badSidCount) // only bad sids -> check if sid is bad
			{
				blocked_sid = is_sid_blocked(ncard, &cur_srvid);
				if(blocked_sid && (!chk_only || blocked_sid->blocked_till == 0))
				{
					continue;
				}
			}
			else if(goodSidCount && badSidCount) // bad and good sids -> check not blocked and good
			{
				blocked_sid = is_sid_blocked(ncard, &cur_srvid);
				good_sid = is_good_sid(ncard, &cur_srvid);

				if(blocked_sid && (!chk_only || blocked_sid->blocked_till == 0))
				{
					continue;
				}

				if(!good_sid)
				{
					continue;
				}
			}

			if(caid_is_nagra(ncard->caid) && (!xcard || ncard->hop < xcard->hop))
			{
				xcard = ncard; // remember card (D+ / 1810 fix) if request has no provider, but card has
			}

			rating = ncard->rating - ncard->hop * HOP_RATING;
			if(rating < MIN_RATING)
			{
				rating = MIN_RATING;
			}
			else if(rating > MAX_RATING)
			{
				rating = MAX_RATING;
			}

			if(!ll_count(ncard->providers)) // card has no providers:
			{
				if(rating > best_rating)
				{
					// ncard is closer
					card = ncard;
					best_rating = rating; // ncard has been matched
				}

			}
			else // card has providers
			{
				LL_ITER it2 = ll_iter_create(ncard->providers);
				struct cc_provider *provider;

				while((provider = ll_iter_next(&it2)))
				{
					if(!cur_er->prid || (provider->prov == cur_er->prid)) // provid matches
					{
						if(rating > best_rating)
						{
							// ncard is closer
							card = ncard;
							best_rating = rating; // ncard has been matched
						}
					}
				}
			}
		}
	}

	if(!card)
	{
		card = xcard; // 18xx: if request has no provider and we have no card, we try this card
	}

	return card;
}

// reopen all blocked sids for this srvid
static void reopen_sids(struct cc_data *cc, int8_t ignore_time, ECM_REQUEST *cur_er, struct cc_srvid *cur_srvid)
{
	time_t utime = time(NULL);
	struct cc_card *card;
	LL_ITER it = ll_iter_create(cc->cards);

	while((card = ll_iter_next(&it)))
	{
		if(card->caid == cur_er->caid) // caid matches
		{
			LL_ITER it2 = ll_iter_create(card->badsids);
			struct cc_srvid_block *srvid;

			while((srvid = ll_iter_next(&it2)))
			{
				if(srvid->blocked_till > 0 && sid_eq((struct cc_srvid *)srvid, cur_srvid))
				{
					if(ignore_time || srvid->blocked_till <= utime)
					{
						ll_iter_remove_data(&it2);
					}
				}
			}
		}
	}
}

static int8_t cc_request_timeout(struct s_client *cl)
{
	struct s_reader *rdr = cl->reader;
	struct cc_data *cc = cl->cc;
	struct timeb timeout;
	struct timeb cur_time;

	if(!cc || !cc->ecm_busy)
	{
		return 0;
	}

	cs_ftime(&cur_time);

	timeout = cc->ecm_time;
	int32_t tt = rdr->cc_reconnect;

	if(tt <= 0)
	{
		tt = DEFAULT_CC_RECONNECT;
	}

	add_ms_to_timeb(&timeout, tt);

	return (comp_timeb(&cur_time, &timeout) >= 0);
}

/**
 * reader
 * sends a ecm request to the connected CCCam Server
 */
int32_t cc_send_ecm(struct s_client *cl, ECM_REQUEST *er)
{
	struct s_reader *rdr = cl->reader;

	//cs_log_dbg(D_TRACE, "%s cc_send_ecm", getprefix());
	if(!rdr->tcp_connected)
	{
		cc_cli_connect(cl);
	}

	int32_t n;
	struct cc_data *cc = cl->cc;
	struct cc_card *card = NULL;
	LL_ITER it;
	ECM_REQUEST *cur_er;
	struct timeb cur_time;
	cs_ftime(&cur_time);

	if(!cc || (cl->pfd < 1) || !rdr->tcp_connected)
	{
		if(er)
		{
			cs_log_dbg(D_READER, "%s server not init! ccinit=%d pfd=%d", rdr->label, cc ? 1 : 0, cl->pfd);
			write_ecm_answer(rdr, er, E_NOTFOUND, E2_CCCAM_NOCARD, NULL, NULL, 0, NULL);
		}
		//cc_cli_close(cl);
		return 0;
	}

	if(rdr->tcp_connected != 2)
	{
		cs_log_dbg(D_READER, "%s Waiting for CARDS", getprefix());
		return 0;
	}

	// No Card? Waiting for shares
	if(!ll_has_elements(cc->cards))
	{
		cs_log_dbg(D_READER, "%s NO CARDS!", getprefix());
		return 0;
	}

	cc->just_logged_in = 0;

	if(!cc->extended_mode)
	{
		// Without extended mode, only one ecm at a time could be send
		// this is a limitation of "O" CCCam
		if(cc->ecm_busy > 0) // Unlock by NOK or ECM ACK
		{
			cs_log_dbg(D_READER, "%s ecm trylock: ecm busy, retrying later after msg-receive", getprefix());

			if(!cc_request_timeout(cl))
			{
				return 0; // pending send...
			}

			if(!cc_cycle_connection(cl))
			{
				return 0;
			}
		}

		cc->ecm_busy = 1;
		cs_log_dbg(D_READER, "cccam: ecm trylock: got lock");
	}

	int32_t processed_ecms = 0;

	do
	{
		cc->ecm_time = cur_time;

		// Search next ECM to send
		if((n = cc_get_nxt_ecm(cl)) < 0)
		{
			if(!cc->extended_mode)
			{
				cc->ecm_busy = 0;
			}

			cs_log_dbg(D_READER, "%s no ecm pending!", getprefix());

			if(!cc_send_pending_emms(cl))
			{
				send_cmd05_answer(cl);
			}

			return 0; // no queued ecms
		}

		cur_er = &cl->ecmtask[n];
		cur_er->rc = E_ALREADY_SENT; // mark ECM as already send
		cs_log_dbg(D_READER, "cccam: ecm-task %d", cur_er->idx);

		// sleepsend support
		static const char *typtext[] = { "ok", "invalid", "sleeping" };

		if(cc->sleepsend && cl->stopped)
		{
			if(cur_er->srvid == cl->lastsrvid && cur_er->caid == cl->lastcaid && cur_er->pid == cl->lastpid)
			{
				cs_log("%s is stopped - requested by server (%s)", cl->reader->label, typtext[cl->stopped]);

				if(!cc->extended_mode)
				{
					cc->ecm_busy = 0;
				}

				write_ecm_answer(rdr, cur_er, E_STOPPED, 0, NULL, NULL, 0, NULL);
				return 0;
			}
			else
			{
				cl->stopped = 0;
			}
		}

		cl->lastsrvid = cur_er->srvid;
		cl->lastcaid = cur_er->caid;
		cl->lastpid = cur_er->pid;
		// sleepsend support end

		struct cc_srvid cur_srvid;
		cur_srvid.sid = cur_er->srvid;
		cur_srvid.chid = cur_er->chid;
		cur_srvid.ecmlen = cur_er->ecmlen;

		cs_readlock(__func__, &cc->cards_busy);

		// forward_origin
		if(cfg.cc_forward_origin_card && cur_er->origin_reader == rdr && cur_er->origin_card)
		{
			it = ll_iter_create(cc->cards);
			struct cc_card *ncard;

			while((ncard = ll_iter_next(&it)))
			{
				if(ncard == cur_er->origin_card) // Search the origin card
				{
					card = ncard; // found it, use it!
					break;
				}
			}
		}

		if(!card)
		{
			reopen_sids(cc, 0, cur_er, &cur_srvid);
			card = get_matching_card(cl, cur_er, 0);
		}

		if(!card && has_srvid(rdr->client, cur_er))
		{
			reopen_sids(cc, 1, cur_er, &cur_srvid);
			card = get_matching_card(cl, cur_er, 0);
		}

		if(card)
		{
			uint8_t *ecmbuf;
			if(!cs_malloc(&ecmbuf, cur_er->ecmlen + 13))
			{
				cs_readunlock(__func__, &cc->cards_busy);
				break;
			}

			// build ecm message
			ecmbuf[0] = cur_er->caid >> 8;
			ecmbuf[1] = cur_er->caid & 0xff;
			ecmbuf[2] = cur_er->prid >> 24;
			ecmbuf[3] = cur_er->prid >> 16;
			ecmbuf[4] = cur_er->prid >> 8;
			ecmbuf[5] = cur_er->prid & 0xff;
			ecmbuf[6] = card->id >> 24;
			ecmbuf[7] = card->id >> 16;
			ecmbuf[8] = card->id >> 8;
			ecmbuf[9] = card->id & 0xff;
			ecmbuf[10] = cur_er->srvid >> 8;
			ecmbuf[11] = cur_er->srvid & 0xff;
			ecmbuf[12] = cur_er->ecmlen & 0xff;
			memcpy(ecmbuf + 13, cur_er->ecm, cur_er->ecmlen);

			uint8_t send_idx = 1;
			if(cc->extended_mode)
			{
				cc->server_ecm_idx++;

				if(cc->server_ecm_idx >= 256)
				{
					cc->server_ecm_idx = 1;
				}

				cc->g_flag = cc->server_ecm_idx; // Flag is used as index!
				send_idx = cc->g_flag;
			}

			struct cc_extended_ecm_idx *eei = get_extended_ecm_idx(cl, send_idx, 0);
			if(eei)
			{
				eei->ecm_idx = cur_er->idx;
				eei->card = card;
				eei->cccam_id = card->id;
				eei->srvid = cur_srvid;
			}
			else
			{
				eei = add_extended_ecm_idx(cl, send_idx, cur_er->idx, card, cur_srvid, 0);
				if(!eei)
				{
					NULLFREE(ecmbuf);
					cs_readunlock(__func__, &cc->cards_busy);
					break;
				}
			}
			eei->tps = cur_er->tps;

			rdr->currenthops = card->hop;
			rdr->card_status = CARD_INSERTED;

			cs_log_dbg( D_READER, "%s sending ecm for sid %04X(%d) to card %08x, hop %d, ecmtask %d",
						getprefix(), cur_er->srvid, cur_er->ecmlen, card->id, card->hop, cur_er->idx);

			cl->reader->last_s = time(NULL);
			cc_cmd_send(cl, ecmbuf, cur_er->ecmlen + 13, MSG_CW_ECM); // send ecm

			NULLFREE(ecmbuf);

			// For EMM
			set_au_data(cl, rdr, card, cur_er);
			cs_readunlock(__func__, &cc->cards_busy);

			processed_ecms++;
			if(cc->extended_mode)
			{
				continue; // process next pending ecm!
			}

			return 0;
		}
		else
		{
			// When connecting, it could happen than ecm requests come before all cards are received.
			// So if the last Message was a MSG_NEW_CARD, this "card receiving" is not already done
			// if this happens, we do not autoblock it and do not set rc status
			// So fallback could resolve it
			if(cc->last_msg != MSG_NEW_CARD && cc->last_msg != MSG_NEW_CARD_SIDINFO
				&& cc->last_msg != MSG_CARD_REMOVED && !cc->just_logged_in)
			{
				cs_log_dbg(D_READER, "%s no suitable card on server", getprefix());

				write_ecm_answer(rdr, cur_er, E_NOTFOUND, E2_CCCAM_NOCARD, NULL, NULL, 0, NULL);

				//cur_er->rc = 1;
				//cur_er->rcEx = 0;
				//cs_sleepms(300);
				rdr->last_s = rdr->last_g;

				reopen_sids(cc, 0, cur_er, &cur_srvid);
			}
			else
			{
				// We didn't find a card and the last message was MSG_CARD_REMOVED,
				// so we wait for a new card and process die ecm later
				cur_er->rc = E_WAITING; // mark as waiting
			}
		}

		cs_readunlock(__func__, &cc->cards_busy);

		// process next pending ecm!
	}
	while(cc->extended_mode || processed_ecms == 0);

	// Now mark all waiting as unprocessed
	int8_t i;
	for(i = 0; i < cfg.max_pending; i++)
	{
		er = &cl->ecmtask[i];
		if(er->rc == E_WAITING)
		{
			er->rc = E_UNHANDLED;
		}
	}

	if(!cc->extended_mode)
	{
		cc->ecm_busy = 0;
	}

	return 0;
}

/*int32_t cc_abort_user_ecms()
{
	int32_t n, i;
	time_t t;//, tls;
	struct cc_data *cc = rdr->cc;

	t = time((time_t *)0);
	for(i = 1, n = 1; i < cfg.max_pending; i++)
	{
		if((t-cl->ecmtask[i].tps.time > ((cfg.ctimeout + 500) / 1000) + 1) && (cl->ecmtask[i].rc >= 10)) // drop timeouts
		{
			cl->ecmtask[i].rc=0;
		}

		int32_t td = abs(comp_timeb(&ecmtask[i].tps, &cc->found->tps);
		if(ecmtask[i].rc >= 10 && ecmtask[i].cidx == cc->found->cidx && &ecmtask[i] != cc->found)
		{
			cs_log("aborting idx:%d caid:%04x client:%d timedelta:%d",ecmtask[i].idx,ecmtask[i].caid,ecmtask[i].cidx,td);
			ecmtask[i].rc = 0;
			ecmtask[i].rcEx = 7;
			write_ecm_answer(rdr, fd_c2m, &ecmtask[i], 0, NULL);
		}
	}
	return n;
}*/

int32_t cc_send_pending_emms(struct s_client *cl)
{
	struct cc_data *cc = cl->cc;
	if(!cc)
	{
		return 0;
	}

	LL_ITER it = ll_iter_create(cc->pending_emms);
	uint8_t *emmbuf;
	int32_t size = 0;

	if((emmbuf = ll_iter_next(&it)))
	{
		if(!cc->extended_mode)
		{
			if(cc->ecm_busy > 0) // Unlock by NOK or ECM ACK
			{
				return 0; // send later with cc_send_ecm
			}
			cc->ecm_busy = 1;
		}

		// Support for emmsize > 256 bytes
		size = (emmbuf[11] | (emmbuf[2] << 8)) + 12;
		emmbuf[2] = 0;

		cc->just_logged_in = 0;
		cs_ftime(&cc->ecm_time);

		cs_log_dbg(D_EMM, "%s emm send for card %08X", getprefix(), b2i(4, emmbuf + 7));

		cc_cmd_send(cl, emmbuf, size, MSG_EMM_ACK); // send emm
		cl->last = time(NULL);
		cl->reader->last_g = time(NULL);
		cl->reader->last_s = time(NULL);

		ll_iter_remove_data(&it);
	}

	return size;
}

/**
 * READER only:
 * find card by hexserial
 */
struct cc_card *get_card_by_hexserial(struct s_client *cl, uint8_t *hexserial, uint16_t caid)
{
	struct cc_data *cc = cl->cc;
	LL_ITER it = ll_iter_create(cc->cards);
	struct cc_card *card;

	while((card = ll_iter_next(&it)))
	{
		if(card->caid == caid && memcmp(card->hexserial, hexserial, 8) == 0) // found it!
		{
			break;
		}
	}

	return card;
}

/**
 * EMM Procession
 * Copied from http://85.17.209.13:6100/file/8ec3c0c5d257/systems/cardclient/cccam2.c
 * ProcessEmm
 */
int32_t cc_send_emm(EMM_PACKET *ep)
{
	struct s_client *cl = cur_client();
	struct s_reader *rdr = cl->reader;

	if(!rdr->tcp_connected)
	{
		cc_cli_connect(cl);
	}

	struct cc_data *cc = cl->cc;

	if(!cc || (cl->pfd < 1) || !rdr->tcp_connected)
	{
		cs_log_dbg(D_READER, "%s server not init! ccinit=%d pfd=%d", getprefix(), cc ? 1 : 0, cl->pfd);
		return 0;
	}

	if(rdr->audisabled)
	{
		cs_log_dbg(D_READER, "%s au is disabled", getprefix());
		return 0;
	}

	uint16_t caid = b2i(2, ep->caid);

	// Last used card is first card of current_cards
	cs_readlock(__func__, &cc->cards_busy);

	struct cc_card *emm_card = cc->last_emm_card;

	if(!emm_card)
	{
		uint8_t hs[8];
		char tmp_dbg[17];

		cc_UA_oscam2cccam(ep->hexserial, hs, caid);

		cs_log_dbg(D_EMM, "%s au info: searching card for caid %04X oscam-UA: %s",
					getprefix(), b2i(2, ep->caid), cs_hexdump(0, ep->hexserial, 8, tmp_dbg, sizeof(tmp_dbg)));

		cs_log_dbg(D_EMM, "%s au info: searching card for caid %04X cccam-UA: %s",
					getprefix(), b2i(2, ep->caid), cs_hexdump(0, hs, 8, tmp_dbg, sizeof(tmp_dbg)));

		emm_card = get_card_by_hexserial(cl, hs, caid);
	}

	if(!emm_card) // Card for emm not found!
	{
		cs_log_dbg(D_EMM, "%s emm for client %8lX not possible, no card found!",
					getprefix(), (unsigned long)ep->client->thread);

		cs_readunlock(__func__, &cc->cards_busy);
		return 0;
	}

	cs_log_dbg(D_EMM, "%s emm received for client %8lX caid %04X for card %08X",
				getprefix(), (unsigned long)ep->client->thread, caid, emm_card->id);

	int32_t size = ep->emmlen + 12;
	uint8_t *emmbuf;

	if(!cs_malloc(&emmbuf, size))
	{
		cs_readunlock(__func__, &cc->cards_busy);
		return 0;
	}

	// build ecm message
	emmbuf[0] = ep->caid[0];
	emmbuf[1] = ep->caid[1];
	emmbuf[2] = ep->emmlen >> 8; // Support for emm len > 256 bytes
	emmbuf[3] = ep->provid[0];
	emmbuf[4] = ep->provid[1];
	emmbuf[5] = ep->provid[2];
	emmbuf[6] = ep->provid[3];
	emmbuf[7] = emm_card->id >> 24;
	emmbuf[8] = emm_card->id >> 16;
	emmbuf[9] = emm_card->id >> 8;
	emmbuf[10] = emm_card->id & 0xff;
	emmbuf[11] = ep->emmlen & 0xff;
	memcpy(emmbuf + 12, ep->emm, ep->emmlen);

	cs_readunlock(__func__, &cc->cards_busy);

	ll_append(cc->pending_emms, emmbuf);
	cc_send_pending_emms(cl);

	return 1;
}

void cc_free_card(struct cc_card *card)
{
	if(!card)
	{
		return;
	}

	ll_destroy_data(&card->providers);
	ll_destroy_data(&card->badsids);
	ll_destroy_data(&card->goodsids);
	ll_destroy_data(&card->remote_nodes);

	add_garbage(card);
}

struct cc_card *cc_get_card_by_id(uint32_t card_id, LLIST *cards)
{
	if(!cards)
	{
		return NULL;
	}

	LL_ITER it = ll_iter_create(cards);
	struct cc_card *card;

	while((card = ll_iter_next(&it)))
	{
		if(card->id == card_id)
		{
			break;
		}
	}

	return card;
}

void cc_free_cardlist(LLIST *card_list, int32_t destroy_list)
{
	if(card_list)
	{
		LL_ITER it = ll_iter_create(card_list);
		struct cc_card *card;

		while((card = ll_iter_next_remove(&it)))
		{
			cc_free_card(card);
		}

		if(destroy_list)
		{
			ll_destroy(&card_list);
		}
	}
}

/**
 * Clears and free the cc datas
 */
void cc_free(struct s_client *cl)
{
	struct cc_data *cc = cl->cc;
	if(!cc)
	{
		return;
	}

	cl->cc = NULL;

	cs_writelock(__func__, &cc->lockcmd);

	cs_log_dbg(D_TRACE, "exit cccam1/3");
	cc_free_cardlist(cc->cards, 1);
	ll_destroy_data(&cc->pending_emms);
	free_extended_ecm_idx(cc);
	ll_destroy_data(&cc->extended_ecm_idx);

	cs_writeunlock(__func__, &cc->lockcmd);

	cs_log_dbg(D_TRACE, "exit cccam2/3");

	add_garbage(cc->prefix);
	add_garbage(cc);

	cs_log_dbg(D_TRACE, "exit cccam3/3");
}

int32_t is_null_dcw(uint8_t *dcw)
{
	int32_t i;
	for(i = 0; i < 15; i++)
		if(dcw[i])
			{ return 0; }
	return 1;
}

/*int32_t is_dcw_corrupted(uint8_t *dcw)
{
	int32_t i;
	int32_t c, cs;

	for(i = 0; i < 16; i += 4)
	{
		c = (dcw[i] + dcw[i + 1] + dcw[i + 2]) & 0xFF;
		cs = dcw[i + 3];

		if(cs != c)
		{
			return (1);
		}
	}
	return 0;
}*/

int32_t check_extended_mode(struct s_client *cl, char *msg)
{
	// Extended mode: if PARTNER String is ending with [PARAM], extended mode is activated
	// For future compatibilty the syntax should be compatible with
	// [PARAM1,PARAM2...PARAMn]
	//
	// EXT: Extended ECM Mode: Multiple ECMs could be send and received
	//                         ECMs are numbered, Flag (byte[0] is the index
	//
	// SID: Exchange of good sids/bad sids activated (like cccam 2.2.x)
	//      card exchange command MSG_NEW_CARD_SIDINFO instead MSG_NEW_CARD is used
	//
	// SLP: Sleepsend supported, like camd35
	//

	struct cc_data *cc = cl->cc;
	char *saveptr1 = NULL;
	int32_t has_param = 0;
	char *p = strtok_r(msg, "[", &saveptr1);

	while(p)
	{
		p = strtok_r(NULL, ",]", &saveptr1);
		if(p && strncmp(p, "EXT", 3) == 0)
		{
			cc->extended_mode = 1;
			cs_log_dbg(D_CLIENT, "%s extended ECM mode", getprefix());
			has_param = 1;
		}
		else if(p && strncmp(p, "SID", 3) == 0)
		{
			cc->cccam220 = 1;
			cs_log_dbg(D_CLIENT, "%s extra SID mode", getprefix());
			has_param = 1;
		}
		else if(p && strncmp(p, "SLP", 3) == 0)
		{
			cc->sleepsend = 1;
			cs_log_dbg(D_CLIENT, "%s sleepsend", getprefix());
			has_param = 1;
		}
#ifdef CS_CACHEEX_AIO
		else if(p && strncmp(p, "LGF", 3) == 0)
		{
			cc->extended_lg_flagged_cws = 1;
			cs_log_dbg(D_CLIENT, "%s lg-flagged CWs", getprefix());
			has_param = 1;
		}
#endif
	}
	return has_param;
}

void cc_idle(void)
{
	struct s_client *cl = cur_client();
	struct s_reader *rdr = cl->reader;
	struct cc_data *cc = cl->cc;

	if(!cl->udp_fd)
	{
		cc_cli_close(cl, 0);
	}

	if(rdr && !rdr->tcp_connected && (rdr->cc_keepalive || (rdr->tcp_ito == -1 && (rdr->last_s != 0 || rdr->last_g != 0))))
	{
		cc_cli_connect(cl);
	}

	if(!rdr || !rdr->tcp_connected || !cl || !cc)
	{
		return;
	}

	time_t now = time(NULL);

	if(rdr->cc_keepalive)
	{
#ifdef CS_CACHEEX_AIO
			if(!cl->cacheex_aio_checked && ((cl->account && cl->account->cacheex.mode > 0) || (cl->reader && cl->reader->cacheex.mode > 0)))
			{
				cc_cacheex_feature_request(cl);
				cl->cacheex_aio_checked = 1;
			}
#endif
		if(cc_cmd_send(cl, NULL, 0, MSG_KEEPALIVE) > 0)
		{
			cs_log_dbg(D_READER, "cccam: keepalive");

			if(cl)
			{
				cl->last = now;
			}

			if(cl->reader)
			{
				cl->reader->last_s = now;
				cl->reader->last_g = now;
			}
		}
		return;
	}
	else
	{
		//cs_log("last_s - now = %d, last_g - now = %d, tcp_ito=%d",
		//		abs(rdr->last_s - now), abs(rdr->last_g - now), rdr->tcp_ito);

		// check inactivity timeout
		if(rdr->tcp_ito > 0)
		{
			// inactivity timeout is entered in seconds in webif!
			if((llabs(rdr->last_s - now) > rdr->tcp_ito) && (llabs(rdr->last_g - now) > rdr->tcp_ito))
			{
				rdr_log_dbg(rdr, D_READER, "inactive_timeout, close connection (fd=%d)", rdr->client->pfd);
				network_tcp_connection_close(rdr, "inactivity");
				return;
			}
		}

		// check read timeout
		int32_t rto = llabs(rdr->last_g - now);
		//cs_log("last_g - now = %d, rto=%d", rto, rdr->tcp_rto);

		// this is also entered in seconds, actually its an receive timeout!
		if(rto > (rdr->tcp_rto) && (rdr->last_g != 0 || rdr->last_s != 0) && rdr->last_s != rdr->last_g)
		{
			rdr_log_dbg(rdr, D_READER, "read timeout, close connection (fd=%d)", rdr->client->pfd);
			network_tcp_connection_close(rdr, "rto");
			return;
		}
	}
}

struct cc_card *read_card(uint8_t *buf, int32_t buflen, int32_t ext)
{
	struct cc_card *card;
	int16_t nprov, nassign = 0, nreject = 0;
	int32_t offset = 21;

	if(buflen < 21)
	{
		return NULL;
	}

	if(!cs_malloc(&card, sizeof(struct cc_card)))
	{
		return NULL;
	}

	card->providers = ll_create("providers");
	card->badsids = ll_create("badsids");
	card->goodsids = ll_create("goodsids");
	card->remote_nodes = ll_create("remote_nodes");
	card->id = b2i(4, buf);
	card->remote_id = b2i(4, buf + 4);
	card->caid = b2i(2, buf + 8);
	card->hop = buf[10];
	card->reshare = buf[11];
	card->is_ext = ext;
	card->card_type = CT_REMOTECARD;
	memcpy(card->hexserial, buf + 12, 8); // HEXSERIAL!!

	//cs_log_dbg(D_CLIENT, "cccam: card %08x added, caid %04X, hop %d, key %s, count %d", card->id, card->caid,
	//			card->hop, cs_hexdump(0, card->hexserial, 8, tmp_dbg, sizeof(tmp_dbg)), ll_count(cc->cards));

	nprov = buf[20];

	if(ext)
	{
		if(buflen < 23)
		{
			cc_free_card(card);
			return NULL;
		}

		nassign = buf[21];
		nreject = buf[22];

		offset += 2;
	}

	if(buflen < (offset + (nprov * 7)))
	{
		cc_free_card(card);
		return NULL;
	}

	int16_t i;
	for(i = 0; i < nprov; i++) // providers
	{
		struct cc_provider *prov;
		if(!cs_malloc(&prov, sizeof(struct cc_provider)))
		{
			break;
		}

		prov->prov = b2i(3, buf + offset);
		if(prov->prov == 0xFFFFFF && caid_is_betacrypt(card->caid))
		{
			prov->prov = i;
		}

		memcpy(prov->sa, buf + offset + 3, 4);
		//cs_log_dbg(D_CLIENT, "      prov %d, %06x, sa %08x", i + 1, prov->prov, b2i(4, prov->sa));

		ll_append(card->providers, prov);
		offset += 7;
	}

	if(ext)
	{
		if(buflen < (offset + (nassign * 2) + (nreject * 2)))
		{
			cc_free_card(card);
			return NULL;
		}

		for(i = 0; i < nassign; i++)
		{
			uint16_t sid = b2i(2, buf + offset);
			//cs_log_dbg(D_CLIENT, "      assigned sid = %04X, added to good sid list", sid);

			struct cc_srvid *srvid;
			if(!cs_malloc(&srvid, sizeof(struct cc_srvid)))
			{
				break;
			}

			srvid->sid = sid;
			srvid->chid = 0;
			srvid->ecmlen = 0;
			ll_append(card->goodsids, srvid);
			offset += 2;
		}

		for(i = 0; i < nreject; i++)
		{
			uint16_t sid = b2i(2, buf + offset);
			//cs_log_dbg(D_CLIENT, "      rejected sid = %04X, added to sid block list", sid);

			struct cc_srvid_block *srvid;
			if(!cs_malloc(&srvid, sizeof(struct cc_srvid_block)))
			{
				break;
			}

			srvid->sid = sid;
			srvid->chid = 0;
			srvid->ecmlen = 0;
			srvid->blocked_till = 0;
			ll_append(card->badsids, srvid);
			offset += 2;
		}
	}

	if(buflen < (offset + 1))
	{
		return card;
	}

	int16_t remote_count = buf[offset];
	offset++;

	if(buflen < (offset + (remote_count * 8)))
	{
		cc_free_card(card);
		return NULL;
	}

	for(i = 0; i < remote_count; i++)
	{
		uint8_t *remote_node;
		if(!cs_malloc(&remote_node, 8))
		{
			break;
		}

		memcpy(remote_node, buf + offset, 8);
		ll_append(card->remote_nodes, remote_node);
		offset += 8;
	}

	return card;
}

void cc_card_removed(struct s_client *cl, uint32_t shareid)
{
	struct cc_data *cc = cl->cc;
	struct cc_card *card;
	LL_ITER it = ll_iter_create(cc->cards);

	while((card = ll_iter_next(&it)))
	{
		if(card->id == shareid) // && card->sub_id == b2i (3, buf + 9)) {
		{
			//cs_log_dbg(D_CLIENT, "cccam: card %08x removed, caid %04X, count %d",
			//			card->id, card->caid, ll_count(cc->cards));

			ll_iter_remove(&it);
			if(cc->last_emm_card == card)
			{
				cc->last_emm_card = NULL;
				cs_log_dbg(D_READER, "%s current card %08x removed!", getprefix(), card->id);
			}

			free_extended_ecm_idx_by_card(cl, card, 1);

			if(card->hop == 1)
			{
				cc->num_hop1--;
			}
			else if(card->hop == 2)
			{
				cc->num_hop2--;
			}
			else
			{
				cc->num_hopx--;
			}

			if(card->reshare == 0)
			{
				cc->num_reshare0--;
			}
			else if(card->reshare == 1)
			{
				cc->num_reshare1--;
			}
			else if(card->reshare == 2)
			{
				cc->num_reshare2--;
			}
			else
			{
				cc->num_resharex--;
			}

			cs_log_dbg(D_TRACE, "%s card removed: id %8X remoteid %8X caid %4X hop %d reshare %d originid %8X cardtype %d",
						getprefix(), card->id, card->remote_id, card->caid, card->hop, card->reshare, card->origin_id, card->card_type);

			cc_free_card(card);
			cc->card_removed_count++;
			//break;
		}
	}
}

void move_card_to_end(struct s_client *cl, struct cc_card *card_to_move)
{
	struct cc_data *cc = cl->cc;
	LL_ITER it = ll_iter_create(cc->cards);
	struct cc_card *card;

	while((card = ll_iter_next(&it)))
	{
		if(card == card_to_move)
		{
			ll_iter_remove(&it);
			break;
		}
	}

	if(card)
	{
		cs_log_dbg(D_READER, "%s Moving card %08X to the end...", getprefix(), card_to_move->id);
		free_extended_ecm_idx_by_card(cl, card, 0);
		ll_append(cc->cards, card_to_move);
	}
}

/*void fix_dcw(uint8_t *dcw)
{
	int32_t i;

	for(i = 0; i < 16; i += 4)
	{
		dcw[i + 3] = (dcw[i] + dcw[i + 1] + dcw[i + 2]) & 0xFF;
	}
}*/

void addParam(char *param, size_t param_sz, char *value)
{
	if (!param_sz) {
		cs_log("ERROR! Sizeof param is zero!");
		return;
	}

	if (param && value) {
		if ((cs_strlen(param) + cs_strlen(value) + 1) < param_sz) {
			if (cs_strlen(param) < 4) {
				cs_strncat(param, value, param_sz);
			}
			else {
				cs_strncat(param, ",", param_sz);
				cs_strncat(param, value, param_sz);
			}
		}
		else {
			cs_log("ERROR! Buffer overflow in addParam!");
		}
	}
	else {
	cs_log("ERROR! Booth param and value pointer NULL!");
	}
}

static void chk_peer_node_for_oscam(struct cc_data *cc)
{
	if(!cc->is_oscam_cccam) // Allready discovered oscam-cccam
	{
		uint16_t sum = 0x1234;
		uint16_t recv_sum = (cc->peer_node_id[6] << 8) | cc->peer_node_id[7];
		int32_t i;

		for(i = 0; i < 6; i++)
		{
			sum += cc->peer_node_id[i];
		}

		// Create special data to detect oscam-cccam
		cc->is_oscam_cccam = sum == recv_sum;
	}
}

#ifdef MODULE_CCCSHARE
static void cc_s_idle(struct s_client *cl)
{
	cs_log_dbg(D_TRACE, "ccc idle %s", username(cl));
	if(cfg.cc_keep_connected)
	{
#ifdef CS_CACHEEX_AIO
			if(!cl->cacheex_aio_checked && ((cl->account && cl->account->cacheex.mode > 0) || (cl->reader && cl->reader->cacheex.mode > 0)))
			{
				cc_cacheex_feature_request(cl);
				cl->cacheex_aio_checked = 1;
			}
#endif
		cc_cmd_send(cl, NULL, 0, MSG_KEEPALIVE);
		cl->last = time(NULL);
	}
	else
	{
		cs_log_dbg(D_CLIENT, "%s keepalive after maxidle is reached", getprefix());
		cs_disconnect_client(cl);
	}
}
#endif

int32_t cc_parse_msg(struct s_client *cl, uint8_t *buf, int32_t l)
{
	struct s_reader *rdr = (cl->typ == 'c') ? NULL : cl->reader;
	int32_t ret = buf[1];
	struct cc_data *cc = cl->cc;
	char tmp_dbg[33];

	if(!cc || cl->kill)
	{
		return -1;
	}

	char msgname[250];
	cs_log_dbg(cl->typ == 'c' ? D_CLIENT : D_READER, "%s parse_msg=%s", getprefix(), cc_get_msgname(buf[1],msgname,sizeof(msgname)));
	uint8_t *data = buf + 4;

	if(l < 4)
	{
		return -1;
	}

	memcpy(&cc->receive_buffer, data, l - 4);
	cc->last_msg = buf[1];

	switch(buf[1])
	{
		case MSG_CLI_DATA:
		{
			cs_log_dbg(D_CLIENT, "cccam: client data ack");
			break;
		}

		case MSG_SRV_DATA:
		{
			l -= 4;
			cs_log_dbg(D_READER, "%s MSG_SRV_DATA (payload=%d, hex=%02X)", getprefix(), l, l);
			data = cc->receive_buffer;

			if(l == 0x48) // 72 bytes: normal server data
			{
				cs_writelock(__func__, &cc->cards_busy);

				cc_free_cardlist(cc->cards, 0);
				free_extended_ecm_idx(cc);
				cc->last_emm_card = NULL;
				cc->num_hop1 = 0;
				cc->num_hop2 = 0;
				cc->num_hopx = 0;
				cc->num_reshare0 = 0;
				cc->num_reshare1 = 0;
				cc->num_reshare2 = 0;
				cc->num_resharex = 0;

				memcpy(cc->peer_node_id, data, 8);
				memcpy(cc->peer_version, data + 8, 8);

				memcpy(cc->cmd0b_aeskey, cc->peer_node_id, 8);
				memcpy(cc->cmd0b_aeskey + 8, cc->peer_version, 8);

				cs_strncpy(cc->remote_version, (char *)data + 8, sizeof(cc->remote_version));
				cs_strncpy(cc->remote_build, (char *)data + 40, sizeof(cc->remote_build));
				cc->remote_build_nr = atoi(cc->remote_build);

				// multics server response
				if(data[33] == 'M' && data[34] == 'C' && data[35] == 'S')
				{
					cc->multics_mode = 2; // multics server finaly confirmed.
					cc->multics_version[0] = data[37];
					cc->multics_version[1] = data[38];
					cs_log_dbg(D_READER, "multics detected: %s!", getprefix());
				}

				cs_writeunlock(__func__, &cc->cards_busy);

				cs_log_dbg(D_READER, "%s remote server %s running v%s (%s)", getprefix(), cs_hexdump(0,
							cc->peer_node_id, 8, tmp_dbg, sizeof(tmp_dbg)), cc->remote_version, cc->remote_build);

				chk_peer_node_for_oscam(cc);
				// Trick: when discovered partner is an Oscam Client, then we send him our version string:
				if(cc->is_oscam_cccam)
				{
					uint8_t token[256];
#ifdef CS_CACHEEX_AIO
					snprintf((char *)token, sizeof(token), "PARTNER: OSCam %s (%s) [EXT,SID,SLP,LGF]",
#else
					snprintf((char *)token, sizeof(token), "PARTNER: OSCam %s (%s) [EXT,SID,SLP]",
#endif
								CS_VERSION, CS_TARGET);

					cc_cmd_send(cl, token, cs_strlen((char *)token) + 1, MSG_CW_NOK1);
				}

				cc->cmd05_mode = MODE_PLAIN;
				//
				// Keyoffset is payload-size:
				//
			}
			else if(l >= 0x00 && l <= 0x0F)
			{
				cs_writelock(__func__, &cc->cards_busy);
				cc->cmd05_offset = l;
				cs_writeunlock(__func__, &cc->cards_busy);
				//
				// 16..43 bytes: RC4 encryption
				//
			}
			else if((l >= 0x10 && l <= 0x1f) || (l >= 0x24 && l <= 0x2b))
			{
				cs_writelock(__func__, &cc->cards_busy);
				cc_init_crypt(&cc->cmd05_cryptkey, data, l);
				cc->cmd05_mode = MODE_RC4_CRYPT;
				cs_writeunlock(__func__, &cc->cards_busy);
				//
				// 32 bytes: set AES128 key for CMD_05, Key=16 bytes offset keyoffset
				//
			}
			else if(l == 0x20)
			{
				cs_writelock(__func__, &cc->cards_busy);
				memcpy(cc->cmd05_aeskey, data + cc->cmd05_offset, 16);
				cc->cmd05_mode = MODE_AES;
				cs_writeunlock(__func__, &cc->cards_busy);
				//
				// 33 bytes: xor-algo mit payload-bytes, offset keyoffset
				//
			}
			else if(l == 0x21)
			{
				cs_writelock(__func__, &cc->cards_busy);
				cc_init_crypt(&cc->cmd05_cryptkey, data + cc->cmd05_offset, l);
				cc->cmd05_mode = MODE_CC_CRYPT;
				cs_writeunlock(__func__, &cc->cards_busy);
				//
				// 34 bytes: cmd_05 plain back
				//
			}
			else if(l == 0x22)
			{
				cs_writelock(__func__, &cc->cards_busy);
				cc->cmd05_mode = MODE_PLAIN;
				cs_writeunlock(__func__, &cc->cards_busy);
				//
				// 35 bytes: Unknown!! 2 256 byte keys exchange
				//
			}
			else if(l == 0x23)
			{
				cs_writelock(__func__, &cc->cards_busy);
				cc->cmd05_mode = MODE_UNKNOWN;
				cs_writeunlock(__func__, &cc->cards_busy);
				cc_cycle_connection(cl);
				//
				// 44 bytes: set aes128 key, Key=16 bytes [Offset=len(password)]
				//
			}
			else if(l == 0x2c)
			{
				cs_writelock(__func__, &cc->cards_busy);
				memcpy(cc->cmd05_aeskey, data + cs_strlen(rdr->r_pwd), 16);
				cc->cmd05_mode = MODE_AES;
				cs_writeunlock(__func__, &cc->cards_busy);
				//
				// 45 bytes: set aes128 key, Key=16 bytes [Offset=len(username)]
				//
			}
			else if(l == 0x2d)
			{
				cs_writelock(__func__, &cc->cards_busy);
				memcpy(cc->cmd05_aeskey, data + cs_strlen(rdr->r_usr), 16);
				cc->cmd05_mode = MODE_AES;
				cs_writeunlock(__func__, &cc->cards_busy);
				//
				//Unknown!!
				//
			}
			else
			{
				cs_log_dbg(D_READER, "%s received improper MSG_SRV_DATA! No change to current mode, mode=%d",
							getprefix(), cc->cmd05_mode);
				break;
			}

			cs_log_dbg(D_READER, "%s MSG_SRV_DATA MODE=%s, len=%d", getprefix(), cmd05_mode_name[cc->cmd05_mode], l);
			break;
		}

		case MSG_NEW_CARD_SIDINFO:
		case MSG_NEW_CARD:
		{
			if(l < 16 || !rdr)
			{
				break;
			}

			uint16_t caid = b2i(2, buf + 12);

			// filter caid == 0 and maxhop
			if(!caid || buf[14] >= rdr->cc_maxhops + 1)
			{
				break;
			}

			// filter mindown
			if(buf[15] < rdr->cc_mindown)
			{
				break;
			}

			// caid check
			if(!chk_ctab(caid, &rdr->ctab))
			{
				break;
			}

			rdr->tcp_connected = 2; // we have card
			rdr->card_status = CARD_INSERTED;

			cs_writelock(__func__, &cc->cards_busy);
			struct cc_card *card = read_card(data, l - 4, buf[1] == MSG_NEW_CARD_SIDINFO);

			if(!card)
			{
				cs_writeunlock(__func__, &cc->cards_busy);
				break;
			}

			card->origin_reader = rdr;
			card->origin_id = card->id;
			card->grp = rdr->grp;
			card->rdr_reshare = rdr->cc_reshare > -1 ? rdr->cc_reshare : cfg.cc_reshare;

			// Check if this card is from us
			LL_ITER it = ll_iter_create(card->remote_nodes);
			uint8_t *node_id;

			while((node_id = ll_iter_next(&it)))
			{
				if(memcmp(node_id, cc_node_id, sizeof(cc_node_id)) == 0) // this card is from us!
				{
					cs_log_dbg(D_READER, "filtered card because of recursive nodeid: id=%08X, caid=%04X", card->id, card->caid);
					cc_free_card(card);
					card = NULL;
					break;
				}
			}

#ifdef MODULE_CCCSHARE
			// Check Ident filter
			if(card)
			{
				if(!chk_ident(&rdr->ftab, card))
				{
					cc_free_card(card);
					card = NULL;
				}
			}
#endif
			if(card)
			{
				// Check if we already have this card
				it = ll_iter_create(cc->cards);
				struct cc_card *old_card;

				while((old_card = ll_iter_next(&it)))
				{
					// We already have this card, delete it
					if(old_card->id == card->id || same_card(old_card, card))
					{
						cc_free_card(card);
						card = old_card;
						break;
					}
				}

				if(!old_card)
				{
					card->card_type = CT_REMOTECARD;
					ll_append(cc->cards, card);
					set_au_data(cl, rdr, card, NULL);
					cc->card_added_count++;
					card->hop++;

					if(card->hop == 1)
					{
						cc->num_hop1++;
					}
					else if(card->hop == 2)
					{
						cc->num_hop2++;
					}
					else
					{
						cc->num_hopx++;
					}

					if(card->reshare == 0)
					{
						cc->num_reshare0++;
					}
					else if(card->reshare == 1)
					{
						cc->num_reshare1++;
					}
					else if(card->reshare == 2)
					{
						cc->num_reshare2++;
					}
					else
					{
						cc->num_resharex++;
					}

					cs_log_dbg(D_TRACE, "%s card added: id %8X remoteid %8X caid %4X hop %d reshare %d originid %8X cardtype %d",
								getprefix(), card->id, card->remote_id, card->caid, card->hop, card->reshare, card->origin_id, card->card_type);
				}
			}

			cs_writeunlock(__func__, &cc->cards_busy);

#ifdef MODULE_CCCSHARE
			cccam_refresh_share();
#endif
			break;
		}

		case MSG_CARD_REMOVED:
		{
			if(l < 8)
			{
				break;
			}

			cs_writelock(__func__, &cc->cards_busy);
			cc_card_removed(cl, b2i(4, buf + 4));
			cs_writeunlock(__func__, &cc->cards_busy);
			break;
		}

		case MSG_SLEEPSEND:
		{
			// Server sends SLEEPSEND
			if(l < 5)
			{
				break;
			}

			if(!cfg.c35_suppresscmd08)
			{
				if(buf[4] == 0xFF)
				{
					cl->stopped = 2; // server says sleep
					//rdr->card_status = NO_CARD;
				}
				else
				{
					if(config_enabled(WITH_LB) && !cfg.lb_mode)
					{
						cl->stopped = 1; // server says invalid
						if(rdr)
							rdr->card_status = CARD_FAILURE;
					}
				}
			}
		} /* fallthrough */ // NO BREAK!! NOK Handling needed!

		case MSG_CW_NOK1:
		case MSG_CW_NOK2:
		{
			if(l < 2)
			{
				break;
			}

			if(l > 5)
			{
				// Received NOK with payload
				char *msg = (char *) buf + 4;

				// Check for PARTNER connection
				if((l >= (4 + 8)) && strncmp(msg, "PARTNER:", 8) == 0)
				{
					// When Data starts with "PARTNER:" we have an Oscam-cccam-compatible client/server!

					cs_strncpy(cc->remote_oscam, msg + 9, sizeof(cc->remote_oscam));
					int32_t has_param = check_extended_mode(cl, msg);

					if(!cc->is_oscam_cccam)
					{
						cc->is_oscam_cccam = 1;

						// send params back. At the moment there is only "EXT"
						char param[20];
						if(!has_param)
						{
							param[0] = 0;
						}
						else
						{
							cs_strncpy(param, " [", sizeof(param));

							if(cc->extended_mode)
							{
								addParam(param, sizeof(param), "EXT");
							}

							if(cc->cccam220)
							{
								addParam(param, sizeof(param), "SID");
							}

							if(cc->sleepsend)
							{
								addParam(param, sizeof(param), "SLP");
							}

#ifdef CS_CACHEEX_AIO
							if(cc->extended_lg_flagged_cws)
							{
								addParam(param, sizeof(param), "LGF");
							}
#endif
							if (!cs_strncat(param, "]", sizeof(param))) {
								cs_log("BUG!!, Adding ']' didn't succed!");
							}
						}

						uint8_t token[256];
						snprintf((char *)token, sizeof(token), "PARTNER: OSCam %s (%s)%s",
								CS_VERSION, CS_TARGET, param);

						cc_cmd_send(cl, token, cs_strlen((char *)token) + 1, MSG_CW_NOK1);
					}
				}
				else
				{
					size_t msg_size = l - 4;
					char last_char = msg[msg_size - 1];

					if(last_char == 0) // verify if the payload is a null terminated string
					{
						if(cs_realloc(&cc->nok_message, msg_size))
						{
							memcpy(cc->nok_message, msg, msg_size);
						}
					}
					else
					{
						NULLFREE(cc->nok_message);
					}
				}

				return ret;
			}

			if(cl->typ == 'c')
			{
				return ret;
			}

			// for reader only

			cc->recv_ecmtask = -1;

			if(cc->just_logged_in) // reader restart needed
			{
				return -1;
			}

			cs_readlock(__func__, &cc->cards_busy);

			struct cc_extended_ecm_idx *eei = get_extended_ecm_idx(cl, cc->extended_mode ? cc->g_flag : 1, 1);
			if(!eei)
			{
				cs_log_dbg(D_READER, "%s received extended ecm NOK id %d but not found!", getprefix(), cc->g_flag);
			}
			else
			{
				uint16_t ecm_idx = eei->ecm_idx;
				cc->recv_ecmtask = ecm_idx;
				struct cc_card *card = eei->card;
				//uint32_t cccam_id = eei->cccam_id;
				struct cc_srvid srvid = eei->srvid;
				int8_t retry = 1;
				struct timeb tpe;
				cs_ftime(&tpe);
				int64_t cwlastresptime = comp_timeb(&tpe, &eei->tps);

				add_garbage(eei);

				if(card)
				{
					if(buf[1] == MSG_CW_NOK1) // MSG_CW_NOK1: share no more available
					{
						cs_log_dbg(D_TRACE, "NOK1: share temporarily not available %d %04X ecm %d %d!",
									card->id, card->caid, eei->send_idx, eei->ecm_idx);

						int j;
						for(j = 0; j < cfg.max_pending; j++)
						{
							if(cl->ecmtask[j].idx == ecm_idx && cl->ecmtask[j].rc == E_ALREADY_SENT)
							{
								ECM_REQUEST *er = &cl->ecmtask[j];
								cl->pending--;

								write_ecm_answer(rdr, er, E_NOTFOUND, 0, NULL, NULL, 0, NULL);
								break;
							}
						}
					}
					else if(cc->cmd05NOK) // else MSG_CW_NOK2: can't decode
					{
						move_card_to_end(cl, card);
						if(cwlastresptime < 5000)
						{
							add_sid_block(card, &srvid, true);
						}
						else
						{
							if(card->rating <= MIN_RATING)
							{
								add_sid_block(card, &srvid, true);
							}
							else
							{
								card->rating--;
							}
						}
					}
					else if(cacheex_get_rdr_mode(rdr) != 1)
					{
						if(!is_good_sid(card, &srvid))
						{
							move_card_to_end(cl, card);
							if(cwlastresptime < 5000)
							{
								add_sid_block(card, &srvid, true);
							}
							else
							{
								if(card->rating <= MIN_RATING)
								{
									add_sid_block(card, &srvid, true);
								}
								else
								{
									card->rating--;
								}
							}
						}
						else
						{
							move_card_to_end(cl, card);
							add_sid_block(card, &srvid, true);
						}

						if(card->rating < MIN_RATING)
						{
							card->rating = MIN_RATING;
						}

						if(cfg.cc_forward_origin_card && card->origin_reader == rdr)
						{
							// this card is from us but it can't decode this ecm
							// also origin card is only set on cccam clients
							// so wie send back the nok to the client
							cs_log_dbg(D_TRACE, "%s forward card: %s", getprefix(), (buf[1] == MSG_CW_NOK1) ? "NOK1" : "NOK2");
							retry = 0;
						}
					}
				}
				else
				{
					cs_log_dbg(D_READER, "%s NOK: NO CARD!", getprefix());
					// try next card...
				}

				// A "NOK" in extended mode means, NOTHING found,
				// regardless of the requested card. So do not retry
				if(cc->extended_mode)
				{
					cl->pending--;
					retry = 0;
				}

				if(retry)
				{
					cc_reset_pending(cl, ecm_idx);
				}
				else
				{
					int32_t i = 0;
					for(i = 0; i < cfg.max_pending; i++)
					{
						if(cl->ecmtask[i].idx == ecm_idx && cl->ecmtask[i].rc == E_ALREADY_SENT)
						{
							cs_log_dbg(D_TRACE, "%s ext NOK %s", getprefix(), (buf[1] == MSG_CW_NOK1) ? "NOK1" : "NOK2");
							ECM_REQUEST *er = &cl->ecmtask[i];
							cl->pending--;

							write_ecm_answer(rdr, er, E_NOTFOUND, 0, NULL, NULL, 0, NULL);
							break;
						}
					}
				}
			}
			cc->cmd05NOK = 0;
			cs_readunlock(__func__, &cc->cards_busy);

			if(!cc->extended_mode)
			{
				cc->ecm_busy = 0;
			}

			cc_send_ecm(cl, NULL);
			break;
		}

		case MSG_CACHE_PUSH:
		{
			if((l - 4) >= 18)
			{
				cc_cacheex_push_in(cl, data);
			}
			break;
		}

		case MSG_CACHE_FILTER:
		{
			if((l - 4) >= 482)
			{
				cc_cacheex_filter_in(cl, data);
			}
			break;
		}
#ifdef CS_CACHEEX_AIO
		case MSG_CACHE_FEATURE_EXCHANGE:
		{
			if((l - 4) >= 2)
			{
				cc_cacheex_feature_request_reply(cl);
			}
			break;
		}

		case MSG_CACHE_FEATURE_EXCHANGE_REPLY:
		{
			if((l - 4) >= 2)
			{
				cc_cacheex_feature_request_save(cl, data);
			}
			break;
		}

		case MSG_CACHE_FEATURE_TRIGGER:
		{
			if((l - 4) >= 2)
			{
				cc_cacheex_feature_trigger_in(cl, data);
			}
			break;
		}

		case MSG_CW_ECM_LGF:
#endif
		case MSG_CW_ECM:
		{
			cc->just_logged_in = 0;
			if(cl->typ == 'c') // SERVER:
			{
#define CCMSG_HEADER_LEN 17
				ECM_REQUEST *er;
				struct cc_card *server_card;

				if(l < CCMSG_HEADER_LEN)
				{
					break;
				}

				if(!cs_malloc(&server_card, sizeof(struct cc_card)))
				{
					break;
				}

				server_card->id = buf[10] << 24 | buf[11] << 16 | buf[12] << 8 | buf[13];
				server_card->caid = b2i(2, data);

				if((er = get_ecmtask()) && l > CCMSG_HEADER_LEN && MAX_ECM_SIZE > l - CCMSG_HEADER_LEN)
				{
					er->caid = b2i(2, buf + 4);
					er->prid = b2i(4, buf + 6);
					er->srvid = b2i(2, buf + 14);
					er->ecmlen = l - CCMSG_HEADER_LEN;
					memcpy(er->ecm, buf + CCMSG_HEADER_LEN, er->ecmlen);
					cc->server_ecm_pending++;
					er->idx = ++cc->server_ecm_idx;

#ifdef MODULE_CCCSHARE
					if(cfg.cc_forward_origin_card) // search my shares for this card:
					{
						cs_log_dbg(D_TRACE, "%s forward card: %04X:%04x search share %d", getprefix(),
									er->caid, er->srvid, server_card->id);

						LLIST **sharelist = get_and_lock_sharelist();
						LL_ITER itr = ll_iter_create(get_cardlist(er->caid, sharelist));
						struct cc_card *card;
						struct cc_card *rcard = NULL;

						while((card = ll_iter_next(&itr)))
						{
							if(card->id == server_card->id) // found it
							{
								break;
							}
						}

						cs_log_dbg(D_TRACE, "%s forward card: share %d found: %d", getprefix(), server_card->id, card ? 1 : 0);

						struct s_reader *ordr = NULL;

						if(card && card->origin_reader) // found own card, now search reader card
						{
							// Search reader in list, because it is maybe offline?
							for(ordr = first_active_reader; ordr; ordr = ordr->next)
							{
								if(ordr == card->origin_reader)
								{
									break;
								}
							}

							if(!ordr)
							{
								cs_log_dbg(D_TRACE, "%s origin reader not found!", getprefix());
							}
							else
							{
								cs_log_dbg(D_TRACE, "%s forward card: share %d origin reader %s origin id %d",
											getprefix(), card->id, ordr->label, card->origin_id);

								struct s_client *cl2 = ordr->client;
								if(card->origin_id && cl2 && cl2->cc) // only if we have a origin from a cccam reader
								{
									struct cc_data *rcc = cl2->cc;

									if(rcc)
									{
										itr = ll_iter_create(rcc->cards);
										while((rcard = ll_iter_next(&itr)))
										{
											if(rcard->id == card->origin_id) // found it!
											{
												break;
											}
										}
									}
								}
								else
								{
									rcard = card;
								}
							}
							er->origin_reader = ordr;
						}

						er->origin_card = rcard;
						if(!rcard || !ordr)
						{
							cs_log_dbg(D_TRACE, "%s forward card: share %d not found!", getprefix(), server_card->id);
							er->rc = E_NOTFOUND;
							er->rcEx = E2_CCCAM_NOK1; // share not found!
						}
						else
						{
							cs_log_dbg(D_TRACE, "%s forward card: share %d forwarded to %s origin as id %d",
										getprefix(), card->id, ordr->label, rcard->id);
						}
						unlock_sharelist();
					}
#endif
					cs_log_dbg(D_CLIENT, "%s ECM request from client: caid %04x srvid %04x(%d) prid %06x",
								getprefix(), er->caid, er->srvid, er->ecmlen, er->prid);

					struct cc_srvid srvid;
					srvid.sid = er->srvid;
					srvid.chid = er->chid;
					srvid.ecmlen = er->ecmlen;
					add_extended_ecm_idx(cl, cc->extended_mode ? cc->g_flag : 1, er->idx, server_card, srvid, 1);

					get_cw(cl, er);

				}
				else
				{
					cs_log_dbg(D_CLIENT, "%s NO ECMTASK!!!! l=%d", getprefix(), l);
					NULLFREE(server_card);
				}

			}
			else // READER
			{
				if(l < 20)
				{
					break;
				}

				cs_readlock(__func__, &cc->cards_busy);
				cc->recv_ecmtask = -1;

				struct cc_extended_ecm_idx *eei = get_extended_ecm_idx(cl, cc->extended_mode ? cc->g_flag : 1, 1);
				if(!eei)
				{
					cs_log_dbg(D_READER, "%s received extended ecm id %d but not found!", getprefix(), cc->g_flag);

					if(!cc->extended_mode)
					{
						cc_cli_close(cl, 0);
					}
				}
				else
				{
					uint16_t ecm_idx = eei->ecm_idx;
					cc->recv_ecmtask = ecm_idx;
					struct cc_card *card = eei->card;
					uint32_t cccam_id = eei->cccam_id;
					struct cc_srvid srvid = eei->srvid;
					NULLFREE(eei);

					if(card)
					{
						if(!cc->extended_mode)
						{
							cc_cw_crypt(cl, buf + 4, card->id);
							cc_crypt_cmd0c(cl, buf + 4, 16);
						}

						memcpy(cc->dcw, buf + 4, 16);
						//fix_dcw(cc->dcw);

						if(!cc->extended_mode) // additional crypto step
						{
							cc_crypt(&cc->block[DECRYPT], buf + 4, l - 4, ENCRYPT);
						}

						if(is_null_dcw(cc->dcw))
						{
							cs_log_dbg(D_READER, "%s null dcw received! sid=%04X(%d)", getprefix(), srvid.sid, srvid.ecmlen);
							move_card_to_end(cl, card);
							add_sid_block(card, &srvid, true);
							// ecm retry
							cc_reset_pending(cl, ecm_idx);
							buf[1] = MSG_CW_NOK2; // So it's really handled like a nok!
						}
						else
						{
							cs_log_dbg(D_READER, "%s cws: %d %s", getprefix(), ecm_idx,
										cs_hexdump(0, cc->dcw, 16, tmp_dbg, sizeof(tmp_dbg)));

							// check response time, if > fallbacktime, switch cards!
							struct timeb tpe;
							cs_ftime(&tpe);
							int64_t cwlastresptime = comp_timeb(&tpe, &cc->ecm_time);

							if(cwlastresptime > get_fallbacktimeout(card->caid) && !cc->extended_mode)
							{
								cs_log_dbg(D_READER, "%s card %04X is too slow, moving to the end...", getprefix(), card->id);
								move_card_to_end(cl, card);

								card->rating--;
								if(card->rating < MIN_RATING)
								{
									card->rating = MIN_RATING;
								}
							}
							else
							{
								card->rating++;
								if(card->rating > MAX_RATING)
								{
									card->rating = MAX_RATING;
								}
							}
						}
					}
					else
					{
						// Card removed...
						cs_log_dbg(D_READER, "%s warning: ECM-CWS respond by CCCam server without current card!", getprefix());

						if(!cc->extended_mode)
						{
							cc_cw_crypt(cl, buf + 4, cccam_id);
							cc_crypt_cmd0c(cl, buf + 4, 16);
						}
						memcpy(cc->dcw, buf + 4, 16);
						//fix_dcw(cc->dcw);

						if(!cc->extended_mode) // additional crypto step
						{
							cc_crypt(&cc->block[DECRYPT], buf + 4, l - 4, ENCRYPT);
						}

						cs_log_dbg(D_READER, "%s cws: %d %s", getprefix(), ecm_idx,
									cs_hexdump(0, cc->dcw, 16, tmp_dbg, sizeof(tmp_dbg)));
					}
				}
				cs_readunlock(__func__, &cc->cards_busy);

				if(!cc->extended_mode)
				{
					cc->ecm_busy = 0;
				}

				//cc_abort_user_ecms();
				cc_send_ecm(cl, NULL);

				if(cc->max_ecms)
				{
					cc->ecm_counter++;
				}
			}
			break;
		}

		case MSG_KEEPALIVE:
		{
#ifdef CS_CACHEEX_AIO
			if(!cl->cacheex_aio_checked && ((cl->account && cl->account->cacheex.mode > 0) || (cl->reader && cl->reader->cacheex.mode > 0)))
			{
				cc_cacheex_feature_request(cl);
				cl->cacheex_aio_checked = 1;
			}
#endif
			if(cl)
			{
				cl->last = time(NULL);
			}

			if(rdr && rdr->cc_keepalive)
			{
				rdr->last_g = time(NULL);
				rdr->last_s = time(NULL);
				rdr_log_dbg(rdr, D_READER, "%s: receive keepalive", __func__);
			}

			cc->just_logged_in = 0;
			break;
		}

		case MSG_CMD_05:
		{
			if(cl->typ != 'c')
			{
				cc->just_logged_in = 0;
				l = l - 4; // Header Length = 4 bytes
				if(l < 0)
				{
					break;
				}

				cs_log_dbg(D_READER, "%s MSG_CMD_05 recvd, payload length=%d mode=%d",
							getprefix(), l, cc->cmd05_mode);

				cc->cmd05_active = 1;
				cc->cmd05_data_len = l;
				memcpy(&cc->cmd05_data, buf + 4, l);

				if(!cc->ecm_busy && ll_has_elements(cc->cards))
				{
					send_cmd05_answer(cl);
				}
			}
			break;
		}

		case MSG_CMD_0B:
		{
			if(l < 20)
			{
				break;
			}

			// by Project: Keynation
			cs_log_dbg(D_READER, "%s MSG_CMD_0B received (payload=%d)!", getprefix(), l - 4);

			AES_KEY key;
			uint8_t aeskey[16];
			uint8_t out[16];

			memcpy(aeskey, cc->cmd0b_aeskey, 16);
			memset(&key, 0, sizeof(key));

			//cs_log_dump_dbg(D_READER, aeskey, 16, "%s CMD_0B AES key:", getprefix());
			//cs_log_dump_dbg(D_READER, data, 16, "%s CMD_0B received data:", getprefix());

			AES_set_encrypt_key((uint8_t *)&aeskey, 128, &key);
			AES_encrypt((uint8_t *)data, (uint8_t *)&out, &key);

			cs_log_dbg(D_TRACE, "%s sending CMD_0B! ", getprefix());
			//cs_log_dump_dbg(D_READER, out, 16, "%s CMD_0B out:", getprefix());
			cc_cmd_send(cl, out, 16, MSG_CMD_0B);
			break;
		}

		case MSG_CMD_0C: // New CCCAM 2.2.0 Server/Client fake check!
		{
			int32_t len = l - 4;
			if(len < 0)
			{
				break;
			}

			if(cl->typ == 'c') // Only im comming from "client"
			{
				cs_log_dbg(D_CLIENT, "%s MSG_CMD_0C received (payload=%d)!", getprefix(), len);

				uint8_t bytes[0x20];
				if(len < 0x20) // if less then 0x20 bytes, clear others
				{
					memset(data + len, 0, 0x20 - len);
				}

				// change first 0x10 bytes to the second
				memcpy(bytes, data + 0x10, 0x10);
				memcpy(bytes + 0x10, data, 0x10);

				// xor data:
				int32_t i;
				for(i = 0; i < 0x20; i++)
				{
					bytes[i] ^= (data[i] & 0x7F);
				}

				// key is now the 16bit hash of md5
				uint8_t md5hash[0x10];
				MD5(data, 0x20, md5hash);
				memcpy(bytes, md5hash, 0x10);

				cs_log_dbg(D_CLIENT, "%s sending CMD_0C! ", getprefix());
				//cs_log_dump_dbg(D_CLIENT, bytes, 0x20, "%s CMD_0C out:", getprefix());
				cc_cmd_send(cl, bytes, 0x20, MSG_CMD_0C);
			}
			else // reader
			{
				// by Project: Keynation + Oscam team
				cc_crypt_cmd0c(cl, data, len);

				uint8_t CMD_0x0C_Command = data[0];

				switch(CMD_0x0C_Command)
				{
					case 0: // RC6
					{
						cc->cmd0c_mode = MODE_CMD_0x0C_RC6;
						break;
					}

					case 1: // RC4
					{
						cc->cmd0c_mode = MODE_CMD_0x0C_RC4;
						break;
					}

					case 2: // CC_CRYPT
					{
						cc->cmd0c_mode = MODE_CMD_0x0C_CC_CRYPT;
						break;
					}

					case 3: // AES
					{
						cc->cmd0c_mode = MODE_CMD_0x0C_AES;
						break;
					}

					case 4: // IDEA
					{
						cc->cmd0c_mode = MODE_CMD_0x0C_IDEA;
						break;
					}

					default:
					{
						cc->cmd0c_mode = MODE_CMD_0x0C_NONE;
					}
				}

				set_cmd0c_cryptkey(cl, data, len);

				cs_log_dbg(D_READER, "%s received MSG_CMD_0C from server! CMD_0x0C_CMD=%d, MODE=%s",
							getprefix(), CMD_0x0C_Command, cmd0c_mode_name[cc->cmd0c_mode]);
			}
			break;
		}

		case MSG_CMD_0D: // key update for the active cmd0x0c algo
		{
			int32_t len = l - 4;
			if(len < 0)
			{
				break;
			}

			if(cc->cmd0c_mode == MODE_CMD_0x0C_NONE)
			{
				break;
			}

			cc_crypt_cmd0c(cl, data, len);
			set_cmd0c_cryptkey(cl, data, len);

			cs_log_dbg(D_READER, "%s received MSG_CMD_0D from server! MODE=%s",
						getprefix(), cmd0c_mode_name[cc->cmd0c_mode]);
			break;
		}

		case MSG_CMD_0E:
		{
			if(l < 2)
			{
				break;
			}

			cs_log_dbg(D_READER, "cccam 2.2.x commands not implemented: 0x%02X", buf[1]);

			// Unkwon commands... need workout algo
			if(cl->typ == 'c') // client connection
			{
				//switching to an oder version and then disconnect...
				cs_strncpy(cfg.cc_version, version[0], sizeof(cfg.cc_version));
				ret = -1;
			}
			else // reader connection
			{
				cs_strncpy(cl->reader->cc_version, version[0], sizeof(cl->reader->cc_version));
				cs_strncpy(cl->reader->cc_build, build[0], sizeof(cl->reader->cc_build));
				cc_cycle_connection(cl);
			}
			break;
		}

		case MSG_EMM_ACK:
		{
			cc->just_logged_in = 0;
			if(cl->typ == 'c') // EMM Request received
			{
				cc_cmd_send(cl, NULL, 0, MSG_EMM_ACK); // Send back ACK

				if(l < 16)
				{
					break;
				}

				cs_log_dbg(D_EMM, "%s EMM Request received!", getprefix());

				if(!ll_count(cl->aureader_list))
				{
					cs_log_dbg( D_EMM, "%s EMM Request discarded because au is not assigned to an reader!", getprefix());
					return MSG_EMM_ACK;
				}

				EMM_PACKET *emm;
				if(!cs_malloc(&emm, sizeof(EMM_PACKET)))
				{
					break;
				}

				emm->caid[0] = buf[4];
				emm->caid[1] = buf[5];
				emm->provid[0] = buf[7];
				emm->provid[1] = buf[8];
				emm->provid[2] = buf[9];
				emm->provid[3] = buf[10];
				//emm->hexserial[0] = buf[11];
				//emm->hexserial[1] = buf[12];
				//emm->hexserial[2] = buf[13];
				//emm->hexserial[3] = buf[14];

				if(l <= 0xFF)
				{
					emm->emmlen = buf[15];
				}
				else
				{
					emm->emmlen = MIN(l - 16, (int32_t)sizeof(emm->emm));
				}

				if(emm->emmlen < 0 || emm->emmlen > MAX_EMM_SIZE || emm->emmlen + 16 > l)
				{
					NULLFREE(emm);
					break;
				}

				memcpy(emm->emm, buf + 16, emm->emmlen);
				//emm->type = UNKNOWN;
				//emm->cidx = cs_idx;
				do_emm(cl, emm);
				NULLFREE(emm);
			}
			else // Our EMM Request Ack!
			{
				cs_log_dbg(D_EMM, "%s EMM ACK!", getprefix());
				if(!cc->extended_mode)
				{
					cc->ecm_busy = 0;
				}
				cc_send_ecm(cl, NULL);
			}
			break;
		}

		default:
		{
			//cs_log_dump_dbg(D_CLIENT, buf, l, "%s unhandled msg: %d len=%d", getprefix(), buf[1], l);
			break;
		}
	}

	if(cc->max_ecms && (cc->ecm_counter > cc->max_ecms))
	{
		cs_log_dbg(D_READER, "%s max ecms (%d) reached, cycle connection!", getprefix(), cc->max_ecms);
		cc_cycle_connection(cl);
	}
	return ret;
}

/**
 * Reader: write dcw to receive
 */
int32_t cc_recv_chk(struct s_client *cl, uint8_t *dcw, int32_t *rc, uint8_t *buf, int32_t UNUSED(n))
{
	struct cc_data *cc = cl->cc;

	if(buf[1] == MSG_CW_ECM
#ifdef CS_CACHEEX_AIO
		 || buf[1] == MSG_CW_ECM_LGF
#endif
	)
	{
		memcpy(dcw, cc->dcw, 16);
		//cs_log_dbg(D_CLIENT, "cccam: recv chk - MSG_CW %d - %s", cc->recv_ecmtask,
		//			cs_hexdump(0, dcw, 16, tmp_dbg, sizeof(tmp_dbg)));
		*rc = 1;
#ifdef CS_CACHEEX_AIO
		if(buf[1] == MSG_CW_ECM_LGF)
			*rc = 0x86;
#endif
		return (cc->recv_ecmtask);
	}
	else if((buf[1] == (MSG_CW_NOK1)) || (buf[1] == (MSG_CW_NOK2)))
	{
		*rc = 0;
		//if(cc->is_oscam_cccam)
		if(cfg.cc_forward_origin_card)
		{
			return (cc->recv_ecmtask);
		}
		else
		{
			return -1;
		}
	}

	return (-1);
}

//int32_t is_softfail(int32_t rc)
//{
//	//see oscam.c send_dcw() for a full list
//	switch(rc)
//	{
//		case 5: // 5 = timeout
//		case 6: // 6 = sleeping
//		case 7: // 7 = fake
//		case 10: // 10 = no card
//		case 11: // 11 = expdate
//		case 12: // 12 = disabled
//		case 13: // 13 = stopped
//		case 14: // 100 = unhandled
//			return 1;
//	}
//	return 0;
//}

/**
 * Server: send DCW to client
 */
void cc_send_dcw(struct s_client *cl, ECM_REQUEST *er)
{
	uint8_t buf[16];
	struct cc_data *cc = cl->cc;

	memset(buf, 0, sizeof(buf));

	struct cc_extended_ecm_idx *eei = get_extended_ecm_idx_by_idx(cl, er->idx, 1);

	if(er->rc < E_NOTFOUND && eei) // found
	{
		memcpy(buf, er->cw, sizeof(buf));
		//fix_dcw(buf);
		//cs_log_dbg(D_TRACE, "%s send cw: %s cpti: %d", getprefix(),
		//			cs_hexdump(0, buf, 16, tmp_dbg, sizeof(tmp_dbg)), er->cpti);

		if(!cc->extended_mode)
		{
			cc_cw_crypt(cl, buf, eei->cccam_id);
		}
		else
		{
			cc->g_flag = eei->send_idx;
		}

#ifdef CS_CACHEEX_AIO
		// lg-flag
		if(cc->extended_lg_flagged_cws && (er->localgenerated || (er->selected_reader && !is_network_reader(er->selected_reader))))
		{
			cc_cmd_send(cl, buf, 16, MSG_CW_ECM_LGF);
		}
		else
		{
#endif
			cc_cmd_send(cl, buf, 16, MSG_CW_ECM);
#ifdef CS_CACHEEX_AIO
		}
#endif

		if(!cc->extended_mode)
		{
			cc_crypt(&cc->block[ENCRYPT], buf, 16, ENCRYPT); // additional crypto step
		}
	}
	else // NOT found
	{
		//cs_log_dbg(D_TRACE, "%s send cw: NOK cpti: %d", getprefix(), er->cpti);

		if(eei && cc->extended_mode)
		{
			cc->g_flag = eei->send_idx;
		}

		int32_t nok, bufsize = 0;
		if(cc->sleepsend && er->rc == E_STOPPED)
		{
			buf[0] = cl->c35_sleepsend;
			bufsize = 1;
			nok = MSG_SLEEPSEND;
		}
		else if(!eei || !eei->card)
		{
			nok = MSG_CW_NOK1; // share no more available
		}
		else
		{
			if(cfg.cc_forward_origin_card && er->origin_card == eei->card)
			{
				nok = (er->rcEx == E2_CCCAM_NOK1) ? MSG_CW_NOK1 : MSG_CW_NOK2;
			}
			else
			{
				nok = MSG_CW_NOK2; // can't decode
			}
		}
		cc_cmd_send(cl, buf, bufsize, nok);
	}
	cc->server_ecm_pending--;

	if(eei)
	{
		NULLFREE(eei->card);
		NULLFREE(eei);
	}
}

int32_t cc_recv(struct s_client *cl, uint8_t *buf, int32_t l)
{
	int32_t n;
	struct s_reader *rdr = (cl->typ == 'c') ? NULL : cl->reader;

	if(buf == NULL || l <= 0)
	{
		return -1;
	}

	n = cc_msg_recv(cl, buf, l); // recv and decrypt msg
	//cs_log_dump_dbg(D_CLIENT, buf, n, "cccam: received %d bytes from %s", n, remote_txt());

	if(n <= 0)
	{
		struct cc_data *cc = cl->cc;
		if(cc && cc->nok_message)
		{
			cs_log_dbg(D_CLIENT, "%s connection closed by %s. n=%d, Reason: %s",
						getprefix(), remote_txt(), n, cc->nok_message);
		}
		else
		{
			cs_log_dbg(D_CLIENT, "%s connection closed by %s, n=%d.", getprefix(), remote_txt(), n);
			if(rdr)
			{
				cc_cli_close(cl, 1);
			}
			else
			{
				//cs_writelock(__func__, &cc->cards_busy); maybe uninitialized
				cs_disconnect_client(cl);
				//cs_writeunlock(__func__, &cc->cards_busy);
			}

			cs_sleepms(150);
			n = -1;
			return n;
		}

		n = -1;
	}
	else if(n < 4)
	{
		cs_log("%s packet is too small (%d bytes)", getprefix(), n);
		n = -1;
	}
	else if(n > CC_MAXMSGSIZE)
	{
		cs_log("%s packet is too big (%d bytes, max: %d)", getprefix(), n, CC_MAXMSGSIZE);
		n = -1;
	}
	else
	{
		// parse it and write it back, if we have received something of value
		n = cc_parse_msg(cl, buf, n);
		if(n == MSG_CW_ECM || n == MSG_EMM_ACK
#ifdef CS_CACHEEX_AIO
			 || n == MSG_CW_ECM_LGF
#endif
		)
		{
			cl->last = time(NULL); // last client action is now
			if(rdr)
			{
				rdr->last_g = time(NULL); // last reader receive is now
			}
		}
	}

	if(n == -1)
	{
		if(cl->typ != 'c')
		{
			cc_cli_close(cl, 1);
		}
	}

	return n;
}

void cc_init_locks(struct cc_data *cc)
{
	cs_lock_create(__func__, &cc->lockcmd, "lockcmd", 5000);
	cs_lock_create(__func__, &cc->cards_busy, "cards_busy", 10000);
}

#ifdef MODULE_CCCSHARE
/**
 * Starting readers to get cards
 **/
int32_t cc_srv_wakeup_readers(struct s_client *cl)
{
	int32_t wakeup = 0;
	struct s_reader *rdr;
	struct s_client *client;

	for(rdr = first_active_reader; rdr; rdr = rdr->next)
	{
		if(rdr->typ != R_CCCAM)
		{
			continue;
		}

		if(rdr->tcp_connected == 2)
		{
			continue;
		}

		if(!(rdr->grp & cl->grp))
		{
			continue;
		}

		// if reader has keepalive but is NOT connected,
		// reader can't connect. so don't ask him
		if(rdr->cc_keepalive)
		{
			continue;
		}

		// reader is in shutdown
		if((client = rdr->client) == NULL || (client->cc) == NULL || client->kill)
		{
			continue;
		}

		// reader cannot be waked up currently because its blocked
		if(is_connect_blocked(rdr))
		{
			continue;
		}

		// This wakeups the reader:
		add_job(rdr->client, ACTION_READER_CARDINFO, NULL, 0);
		wakeup++;
	}

	return wakeup;
}

int32_t cc_srv_connect(struct s_client *cl)
{
	int32_t i, ccversion_pos, ccbuild_pos;
	int32_t no_delay = 1;
	uint8_t data[16];
	char usr[21], pwd[65], tmp_dbg[17];
	struct s_auth *account;
	struct cc_data *cc;

	if(!cs_malloc(&cc, sizeof(struct cc_data)))
	{
		return -1;
	}

	memset(usr, 0, sizeof(usr));
	memset(pwd, 0, sizeof(pwd));

	// init internals data struct
	cl->cc = cc;
	cc->extended_ecm_idx = ll_create("extended_ecm_idx");

	cc_init_locks(cc);
	uint8_t *buf = cc->send_buffer;

	cc->server_ecm_pending = 0;
	cc->extended_mode = 0;
	cc->ecm_busy = 0;

	int32_t keep_alive = 1;
	setsockopt(cl->udp_fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keep_alive, sizeof(keep_alive));

	// Create checksum for "O" cccam
	get_random_bytes(data, 12);
	for(i = 0; i < 4; i++)
	{
		data[12 + i] = (data[i] + data[4 + i] + data[8 + i]) & 0xff;
	}

	cs_log_dbg(D_TRACE, "send ccc checksum");

	send(cl->udp_fd, data, 16, 0);

	cc_xor(data); // XOR init bytes with 'CCcam'

	SHA_CTX ctx;
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, data, 16);
	SHA1_Final(buf, &ctx);

	cc_init_crypt(&cc->block[ENCRYPT], buf, 20);
	cc_crypt(&cc->block[ENCRYPT], data, 16, DECRYPT);
	cc_init_crypt(&cc->block[DECRYPT], data, 16);
	cc_crypt(&cc->block[DECRYPT], buf, 20, DECRYPT);

	cs_log_dbg(D_TRACE, "receive ccc checksum");

	if(cc_recv_to(cl, buf, 20) == 20)
	{
		//cs_log_dump_dbg(D_CLIENT, buf, 20, "cccam: recv:");
		cc_crypt(&cc->block[DECRYPT], buf, 20, DECRYPT);
		//cs_log_dump_dbg(D_CLIENT, buf, 20, "cccam: hash:");
	}
	else
	{
		return -1;
	}

	// receive username
	memset(buf, 0, CC_MAXMSGSIZE);
	i = cc_recv_to(cl, buf, 20);
	if(i < 0) // errors during receive!
	{
		return -1;
	}

	if(i == 20)
	{
		cc_crypt(&cc->block[DECRYPT], buf, 20, DECRYPT);
		cs_strncpy(usr, (char *)buf, sizeof(usr));

		// test for non printable characters
		for(i = 0; i < 20; i++)
		{
			if(usr[i] > 0 && usr[i] < 0x20) // found non printable char
			{
				cs_log("illegal username received");
				return -3;
			}
		}
		//cs_log_dump_dbg(D_CLIENT, buf, 20, "cccam: username '%s':", usr);
	}
	else
	{
		cs_add_violation(cl, NULL);
		return -2;
	}
	cs_log_dbg(D_TRACE, "ccc username received %s", usr);

	cl->crypted = 1;

	// CCCam only supports len=20 usr/pass. So we could have
	// more than one user that matches the first 20 chars.

	// receive password-CCCam encrypted Hash:
	i = cc_recv_to(cl, buf, 6);

	if(i < 0) // errors during receive!
	{
		return -1;
	}

	if(i != 6) // received invalid password length
	{
		cs_add_violation(cl, usr);
		return -2;
	}

	cs_log_dbg(D_TRACE, "ccc passwdhash received %s", usr);

	account = cfg.account;
	struct cc_crypt_block *save_block;
	if(!cs_malloc(&save_block, sizeof(struct cc_crypt_block)))
	{
		return -1;
	}

	memcpy(save_block, cc->block, sizeof(struct cc_crypt_block));
	int32_t found = 0;

	while(1)
	{
		while(account)
		{
			if(strncmp(usr, account->usr, 20) == 0)
			{
				memset(pwd, 0, sizeof(pwd));
				cs_strncpy(pwd, account->pwd, sizeof(pwd));
				found = 1;
				break;
			}
			account = account->next;
		}

		if(!account)
		{
			break;
		}

		// receive passwd / 'CCcam'
		memcpy(cc->block, save_block, sizeof(struct cc_crypt_block));
		cc_crypt(&cc->block[DECRYPT], (uint8_t *) pwd, cs_strlen(pwd), ENCRYPT);
		cc_crypt(&cc->block[DECRYPT], buf, 6, DECRYPT);

		// illegal buf-bytes could kill the logger!
		//cs_log_dump_dbg(D_CLIENT, buf, 6, "cccam: pwd check '%s':", buf);

		if(memcmp(buf, "CCcam\0", 6) == 0) // Password Hash OK!
		{
			break; // account is set
		}

		account = account->next;
	}
	NULLFREE(save_block);

	// cs_auth_client returns 0 if account is valid/active/accessible
	if(cs_auth_client(cl, account, NULL))
	{
		if(!found)
		{
			cs_log("account '%s' not found!", usr);
		}
		else
		{
			cs_log("password for '%s' invalid!", usr);
		}

		cs_add_violation(cl, usr);
		return -2;
	}

	if(cl->dup)
	{
		cs_log("account '%s' duplicate login, disconnect!", usr);
		return -3;
	}

	if(cl->disabled)
	{
		cs_log("account '%s' disabled, blocking+disconnect!", usr);
		cs_add_violation(cl, usr);
		return -2;
	}

	if(account->cccmaxhops < -1)
	{
		cs_log("account '%s' has cccmaxhops < -1, cccam can't handle this, disconnect!", usr);
		return -3;
	}

	cs_log_dbg(D_TRACE, "ccc user authenticated %s", usr);

	if(account->cccmaxhops == -1)
	{
		cs_log("account '%s' has cccmaxhops = -1: user will not see any card!", usr);
	}

	if(!cs_malloc(&cc->prefix, cs_strlen(cl->account->usr) + 20))
	{
		return -1;
	}
	snprintf(cc->prefix, cs_strlen(cl->account->usr) + 20, "cccam(s) %s:", cl->account->usr);

#ifdef CS_CACHEEX
	if(cl->account->cacheex.mode < 2)
#endif
	if(cl->tcp_nodelay == 0)
	{
		setsockopt(cl->udp_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&no_delay, sizeof(no_delay));
		cl->tcp_nodelay = 1;
	}

	// Starting readers to get cards
	cc_srv_wakeup_readers(cl);

	// send passwd ack
	memset(buf, 0, 20);
	memcpy(buf, "CCcam\0", 6);
	cs_log_dump_dbg(D_CLIENT, buf, 20, "cccam: send ack:");
	cc_crypt(&cc->block[ENCRYPT], buf, 20, ENCRYPT);
	send(cl->pfd, buf, 20, 0);

	// recv cli data
	memset(buf, 0, CC_MAXMSGSIZE);
	i = cc_msg_recv(cl, buf, CC_MAXMSGSIZE - 1);

	if(i < 0)
	{
		return -1;
	}
	cs_log_dump_dbg(D_CLIENT, buf, i, "cccam: cli data:");

	if(i < 66)
	{
		cs_log_dbg(D_CLIENT, "cccam: cli data too small");
		return -1;
	}

	memcpy(cc->peer_node_id, buf + 24, 8);
	//chk_peer_node_for_oscam(cc);

	ccversion_pos = 33;
	while(ccversion_pos + 1 < i && ccversion_pos < 33 + 5 && buf[ccversion_pos] == 0)
	{
		ccversion_pos++;
	}

	ccbuild_pos = 65;
	while(ccbuild_pos + 1 < i && ccbuild_pos < 65 + 5 && buf[ccbuild_pos] == 0)
	{
		ccbuild_pos++;
	}

	cs_strncpy(cc->remote_version, (char *)buf + ccversion_pos, sizeof(cc->remote_version));
	cs_strncpy(cc->remote_build, (char *)buf + ccbuild_pos, sizeof(cc->remote_build));

	cs_log_dbg(D_CLIENT, "%s client '%s' (%s) running v%s (%s)", getprefix(), buf + 4,
				cs_hexdump(0, cc->peer_node_id, 8, tmp_dbg, sizeof(tmp_dbg)), cc->remote_version, cc->remote_build);

	// send cli data ack
	cc_cmd_send(cl, NULL, 0, MSG_CLI_DATA);

	cs_log_dbg(D_TRACE, "ccc send srv_data %s", usr);
	if(cc_send_srv_data(cl) < 0)
	{
		return -1;
	}

	cc->cccam220 = check_cccam_compat(cc);
	cc->just_logged_in = 1;

	// Wait for Partner detection (NOK1 with data) before reporting cards
	// When Partner is detected, cccam220=1 is set. then we can report extended card data
	i = process_input(buf, CC_MAXMSGSIZE, 1);

	if(i <= 0 && i != -9) // disconnected
	{
		return 0;
	}

	if(cc->cccam220)
	{
		cs_log_dbg(D_CLIENT, "%s extended sid mode activated", getprefix());
	}
	else
	{
		cs_log_dbg(D_CLIENT, "%s 2.1.x compatibility mode", getprefix());
	}

	cs_log_dbg(D_TRACE, "ccc send cards %s", usr);

	if(!cc_srv_report_cards(cl))
	{
		return -1;
	}
	cs_ftime(&cc->ecm_time);

	// some clients, e.g. mgcamd, do not support keepalive. So if not answered, keep
	// connection check for client timeout. If timeout occurs try to send keepalive
	cs_log_dbg(D_TRACE, "ccc connected and waiting for data %s", usr);
	return 0;
}

void cc_srv_init2(struct s_client *cl)
{
	if(!cl->init_done && !cl->kill)
	{
		if(IP_ISSET(cl->ip))
		{
			cs_log_dbg(D_CLIENT, "cccam: new connection from %s", cs_inet_ntoa(cl->ip));
		}

		cl->pfd = cl->udp_fd;
		int32_t ret;
		if((ret = cc_srv_connect(cl)) < 0)
		{
			if(errno != 0)
			{
				cs_log_dbg(D_CLIENT, "cccam: failed errno: %d (%s)", errno, strerror(errno));
			}
			else
			{
				cs_log_dbg(D_CLIENT, "cccam: failed ret: %d", ret);
			}
			cs_disconnect_client(cl);
		}
		else
		{
			cl->init_done = 1;
			cc_cacheex_filter_out(cl);
#ifdef CS_CACHEEX_AIO
			if((cl->account && cl->account->cacheex.mode > 0) || (cl->reader && cl->reader->cacheex.mode > 0))
				cc_cacheex_feature_request(cl);
#endif
		}
	}
	return;
}

void *cc_srv_init(struct s_client *cl, uint8_t *UNUSED(mbuf), int32_t UNUSED(len))
{
	cc_srv_init2(cl);
	return NULL;
}
#endif

int32_t cc_cli_connect(struct s_client *cl)
{
	struct s_reader *rdr = cl->reader;
	struct cc_data *cc = cl->cc;
	rdr->card_status = CARD_FAILURE;
	cl->stopped = 0;

	if(!cc)
	{
		// init internals data struct
		if(!cs_malloc(&cc, sizeof(struct cc_data)))
		{
			return -1;
		}

		cc_init_locks(cc);
		cc->cards = ll_create("cards");
		cl->cc = cc;
		cc->pending_emms = ll_create("pending_emms");
		cc->extended_ecm_idx = ll_create("extended_ecm_idx");
	}
	else
	{
		cc_free_cardlist(cc->cards, 0);
		free_extended_ecm_idx(cc);
	}

	if(!cc->prefix)
	{
		if(!cs_malloc(&cc->prefix, cs_strlen(cl->reader->label) + 20))
		{
			return -1;
		}
	}
	snprintf(cc->prefix, cs_strlen(cl->reader->label) + 20, "cccam(r) %s:", cl->reader->label);

	int32_t handle, n;
	uint8_t data[20];
	uint8_t hash[SHA_DIGEST_LENGTH];
	uint8_t *buf = cc->send_buffer;
	char pwd[65];

	// check cred config
	if(rdr->device[0] == 0 || rdr->r_pwd[0] == 0 || rdr->r_usr[0] == 0 || rdr->r_port == 0)
	{
		cs_log("%s configuration error!", rdr->label);
		return -5;
	}

	// connect
	handle = network_tcp_connection_open(rdr);
	if(handle <= 0)
	{
		cs_log_dbg(D_READER, "%s network connect error!", rdr->label);
		return -1;
	}

	if(errno == EISCONN)
	{
		cc_cli_close(cl, 0);
		block_connect(rdr);
		return -1;
	}

	int32_t no_delay = 1;
	if(cacheex_get_rdr_mode(rdr) < 2)
	{
		setsockopt(cl->udp_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&no_delay, sizeof(no_delay));
	}

	// get init seed
	if((n = cc_recv_to(cl, data, 16)) != 16)
	{
		if(n <= 0)
		{
			cs_log("init error from reader %s", rdr->label);
		}
		else
		{
			cs_log("%s server returned %d instead of 16 bytes as init seed (errno=%d %s)",
					rdr->label, n, errno, strerror(errno));
		}

		cc_cli_close(cl, 0);
		block_connect(rdr);
		return -2;
	}

	cc->ecm_counter = 0;
	cc->max_ecms = 0;
	cc->cmd05_mode = MODE_UNKNOWN;
	cc->cmd05_offset = 0;
	cc->cmd05_active = 0;
	cc->cmd05_data_len = 0;
	cc->extended_mode = 0;
	cc->last_emm_card = NULL;
	cc->num_hop1 = 0;
	cc->num_hop2 = 0;
	cc->num_hopx = 0;
	cc->num_reshare0 = 0;
	cc->num_reshare1 = 0;
	cc->num_reshare2 = 0;
	cc->num_resharex = 0;
	memset(&cc->cmd05_data, 0, sizeof(cc->cmd05_data));
	memset(&cc->receive_buffer, 0, sizeof(cc->receive_buffer));
	NULLFREE(cc->nok_message);
	cc->cmd0c_mode = MODE_CMD_0x0C_NONE;

	cs_log_dump_dbg(D_CLIENT, data, 16, "cccam: server init seed:");

	uint16_t sum = 0x1234;
	uint16_t recv_sum = (data[14] << 8) | data[15];
	int32_t i;

	for(i = 0; i < 14; i++)
	{
		sum += data[i];
	}

	// create special data to detect oscam-cccam
	cc->is_oscam_cccam = sum == recv_sum;

	// detect multics seed
	uint8_t a = (data[0] ^ 'M') + data[1] + data[2];
	uint8_t b = data[4] + (data[5] ^ 'C') + data[6];
	uint8_t c = data[8] + data[9] + (data[10] ^ 'S');

	if((a == data[3]) && (b == data[7]) && (c == data[11]))
	{
		cc->multics_mode = 1; // detected multics seed
		cs_log_dbg(D_READER, "multics seed detected: %s", rdr->label);
	}

	cc_xor(data); // XOR init bytes with 'CCcam'

	SHA_CTX ctx;
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, data, 16);
	SHA1_Final(hash, &ctx);

	cs_log_dump_dbg(D_CLIENT, hash, sizeof(hash), "cccam: sha1 hash:");

	// initialisate crypto states
	cc_init_crypt(&cc->block[DECRYPT], hash, 20);
	cc_crypt(&cc->block[DECRYPT], data, 16, DECRYPT);
	cc_init_crypt(&cc->block[ENCRYPT], data, 16);
	cc_crypt(&cc->block[ENCRYPT], hash, 20, DECRYPT);

	cc_cmd_send(cl, hash, 20, MSG_NO_HEADER); // send crypted hash to server

	memset(buf, 0, CC_MAXMSGSIZE);
	memcpy(buf, rdr->r_usr, cs_strlen(rdr->r_usr));
	cs_log_dump_dbg(D_CLIENT, buf, 20, "cccam: username '%s':", buf);
	cc_cmd_send(cl, buf, 20, MSG_NO_HEADER); // send usr '0' padded -> 20 bytes

	memset(buf, 0, CC_MAXMSGSIZE);
	memset(pwd, 0, sizeof(pwd));

	//cs_log_dbg(D_CLIENT, "cccam: 'CCcam' xor");
	memcpy(buf, "CCcam", 5);
	cs_strncpy(pwd, rdr->r_pwd, sizeof(pwd));
	cc_crypt(&cc->block[ENCRYPT], (uint8_t *)pwd, cs_strlen(pwd), ENCRYPT);
	cc_cmd_send(cl, buf, 6, MSG_NO_HEADER); // send 'CCcam' xor w/ pwd

	if((cc_recv_to(cl, data, 20)) != 20)
	{
		cs_log("%s login failed, usr/pwd invalid", getprefix());
		cc_cli_close(cl, 0);
		block_connect(rdr);
		return -2;
	}

	cc_crypt(&cc->block[DECRYPT], data, 20, DECRYPT);
	cs_log_dump_dbg(D_CLIENT, data, 20, "cccam: login data");

	if(memcmp(data, buf, 5)) // check server response
	{
		cs_log("%s login failed, usr/pwd invalid", getprefix());
		cc_cli_close(cl, 0);
		block_connect(rdr);
		return -2;
	}
	else
	{
		cs_log_dbg(D_READER, "%s login succeeded", getprefix());
	}

	cs_log_dbg(D_READER, "cccam: last_s=%ld, last_g=%ld", rdr->last_s, rdr->last_g);

	cl->pfd = cl->udp_fd;
	cs_log_dbg(D_READER, "cccam: pfd=%d", cl->pfd);

	if(cc_send_cli_data(cl) <= 0)
	{
		cs_log("%s login failed, could not send client data", getprefix());
		cc_cli_close(cl, 0);
		block_connect(rdr);
		return -3;
	}

	if(rdr->ftab.filts)
	{
		rdr->caid = rdr->ftab.filts[0].caid;
		rdr->nprov = rdr->ftab.filts[0].nprids;

		for(n = 0; n < rdr->nprov; n++)
		{
			rdr->prid[n][0] = rdr->ftab.filts[0].prids[n] >> 24;
			rdr->prid[n][1] = rdr->ftab.filts[0].prids[n] >> 16;
			rdr->prid[n][2] = rdr->ftab.filts[0].prids[n] >> 8;
			rdr->prid[n][3] = rdr->ftab.filts[0].prids[n] & 0xff;
		}
	}

	rdr->card_status = CARD_NEED_INIT;
	rdr->last_g = rdr->last_s = time((time_t *) 0);
	rdr->tcp_connected = 1;

	cc->just_logged_in = 1;
	cl->crypted = 1;
	cc->ecm_busy = 0;

#ifdef CS_CACHEEX_AIO
	if(cacheex_get_rdr_mode(rdr) > 0)
	{
#endif

		cc_cacheex_filter_out(cl);

#ifdef CS_CACHEEX_AIO
		cc_cacheex_feature_request(cl);
	}
#endif

	return 0;
}

int32_t cc_cli_init_int(struct s_client *cl)
{
	struct s_reader *rdr = cl->reader;

	if(rdr->tcp_connected)
	{
		return 1;
	}

	if(rdr->tcp_ito < 15 && rdr->tcp_ito !=-1)
	{
		rdr->tcp_ito = 30;
	}

	if(rdr->cc_maxhops < 0)
	{
		rdr->cc_maxhops = DEFAULT_CC_MAXHOPS;
	}

	if(rdr->tcp_rto < 1) // timeout to 30s
	{
		rdr->tcp_rto = 30;
	}

	cs_log_dbg(D_READER, "cccam: inactivity timeout: %d seconds, receive timeout: %d seconds",
				rdr->tcp_ito, rdr->tcp_rto);

	cc_check_version(rdr->cc_version, rdr->cc_build);

	cs_log_dbg(D_READER, "proxy reader: %s (%s:%d) cccam v%s build %s, maxhops: %d",
				rdr->label, rdr->device, rdr->r_port, rdr->cc_version, rdr->cc_build, rdr->cc_maxhops);

	return 0;
}

int32_t cc_cli_init(struct s_client *cl)
{
	struct s_reader *reader = cl->reader;
	int32_t res = cc_cli_init_int(cl); // Create socket

	if(res == 0 && reader && (reader->cc_keepalive || !cl->cc) && !reader->tcp_connected)
	{
		cc_cli_connect(cl); // connect to remote server

		//while(!reader->tcp_connected && reader->cc_keepalive && cfg.reader_restart_seconds > 0)
		//{
		//	if((cc && cc->mode == CCCAM_MODE_SHUTDOWN))
		//	{
		//		return -1;
		//	}
		//
		//	if(!reader->tcp_connected)
		//	{
		//		cc_cli_close(cl, 0);
		//		res = cc_cli_init_int(cl);
		//		if(res)
		//		{
		//			return res;
		//		}
		//	}
		//
		//	cs_log_dbg(D_READER, "%s restarting reader in %d seconds", reader->label, cfg.reader_restart_seconds);
		//	cs_sleepms(cfg.reader_restart_seconds*1000);
		//	cs_log_dbg(D_READER, "%s restarting reader...", reader->label);
		//	cc_cli_connect(cl);
		//}
	}
	return res;
}

/**
 * return 1 if we are able to send requests:
 */
int32_t cc_available(struct s_reader *rdr, int32_t checktype, ECM_REQUEST *er)
{
	if(!rdr || !rdr->client)
	{
		return 0;
	}

	struct s_client *cl = rdr->client;
	if(!cl)
	{
		return 0;
	}

	struct cc_data *cc = cl->cc;
	if(er && cc && rdr->tcp_connected)
	{
		struct cc_card *card = get_matching_card(cl, er, 1);
		if(!card)
		{
			return 0;
		}
	}
	//cs_log_dbg(D_TRACE, "checking reader %s availibility", rdr->label);

	if(!cc || rdr->tcp_connected != 2)
	{
		// Two cases:
		// 1. Keepalive ON but not connected: Do NOT send requests,
		//     because we can't connect - problem of full running pipes
		// 2. Keepalive OFF but not connected: Send requests to connect
		//     pipe won't run full, because we are reading from pipe to
		//     get the ecm request

		if(rdr->cc_keepalive)
		{
			return 0;
		}
	}

	//if(er && er->ecmlen > 255 && cc && !cc->extended_mode && (cc->remote_build_nr < 3367))
	//{
	//	return 0; // remote does not support large ecms!
	//}

	if(checktype == AVAIL_CHECK_LOADBALANCE && cc && cc->ecm_busy)
	{
		if(cc_request_timeout(cl))
		{
			cc_cycle_connection(cl);
		}

		if(!rdr->tcp_connected || cc->ecm_busy)
		{
			cs_log_dbg(D_TRACE, "checking reader %s availibility=0 (unavail)", rdr->label);
			return 0; // We are processing EMMs/ECMs
		}
	}

	return 1;
}

/**
 *
 **/
void cc_card_info(void)
{
	struct s_client *cl = cur_client();
	struct s_reader *rdr = cl->reader;

	if(rdr && !rdr->tcp_connected)
	{
		cc_cli_connect(cl);
	}
}

void cc_cleanup(struct s_client *cl)
{
	if(cl->typ != 'c')
	{
		cc_cli_close(cl, 1); // we need to close open fd's
	}
	cc_free(cl);
}

void cc_update_nodeid(void)
{
	if(array_has_nonzero_byte(cfg.cc_fixed_nodeid, sizeof(cfg.cc_fixed_nodeid)))
	{
		memcpy(cc_node_id, cfg.cc_fixed_nodeid, 8);
		return;
	}

	// Partner Detection
	uint16_t sum = 0x1234; // This is our checksum
	int32_t i;
	get_random_bytes(cc_node_id, 4);

	for(i = 0; i < 4; i++)
	{
		sum += cc_node_id[i];
	}

	// Partner ID
	cc_node_id[4] = 0x10; // (Oscam 0x10, vPlugServer 0x11, Hadu 0x12, ...)
	sum += cc_node_id[4];

	// generate checksum for Partner ID:
	cc_node_id[5] = 0xAA;

	for(i = 0; i < 5; i++)
	{
		cc_node_id[5] ^= cc_node_id[i];
	}
	sum += cc_node_id[5];

	cc_node_id[6] = sum >> 8;
	cc_node_id[7] = sum & 0xff;

	memcpy(cfg.cc_fixed_nodeid, cc_node_id, 8);
}

bool cccam_forward_origin_card(ECM_REQUEST *er)
{
	if(cfg.cc_forward_origin_card && er->origin_card)
	{
		struct cc_card *card = er->origin_card;
		struct s_ecm_answer *eab = NULL;
		struct s_ecm_answer *ea;

		for(ea = er->matching_rdr; ea; ea = ea->next)
		{
			ea->status &= ~(READER_ACTIVE | READER_FALLBACK);
			if(card->origin_reader == ea->reader)
			{
				eab = ea;
			}
		}

		if(eab)
		{
			cs_log_dbg(D_LB, "loadbalancer: forward card: forced by card %d to reader %s",
						card->id, eab->reader->label);

			eab->status |= READER_ACTIVE;
			return true;
		}
	}

	return false;
}

bool cccam_snprintf_cards_stat(struct s_client *cl, char *emmtext, size_t emmtext_sz)
{
	struct cc_data *rcc = cl->cc;
	if(rcc)
	{
		LLIST *cards = rcc->cards;
		if(cards)
		{
			int32_t ncards = ll_count(cards);
			int32_t locals = rcc->num_hop1;
			snprintf(emmtext, emmtext_sz, " %3d/%3d card%s", locals, ncards, ncards > 1 ? "s " : "  ");
			return true;
		}
	}
	return false;
}

bool cccam_client_extended_mode(struct s_client *cl)
{
	return cl && cl->cc && ((struct cc_data *)cl->cc)->extended_mode;
}

bool cccam_client_multics_mode(struct s_client *cl)
{
	return cl && cl->cc && ((struct cc_data *)cl->cc)->multics_mode == 2;
}

void module_cccam(struct s_module *ph)
{
	ph->desc = "cccam";
	ph->type = MOD_CONN_TCP;
	ph->large_ecm_support = 1;
	ph->listenertype = LIS_CCCAM;
	ph->num = R_CCCAM;
	ph->recv = cc_recv;
	ph->cleanup = cc_cleanup;
	ph->bufsize = 2048;
	ph->c_init = cc_cli_init;
	ph->c_idle = cc_idle;
	ph->c_recv_chk = cc_recv_chk;
	ph->c_send_ecm = cc_send_ecm;
	ph->c_send_emm = cc_send_emm;
#ifdef MODULE_CCCSHARE
	IP_ASSIGN(ph->s_ip, cfg.cc_srvip);
	ph->s_handler = cc_srv_init;
	ph->s_init = cc_srv_init2;
	ph->s_idle = cc_s_idle;
	ph->send_dcw = cc_send_dcw;
#endif
	ph->c_available = cc_available;
	ph->c_card_info = cc_card_info;
	cc_cacheex_module_init(ph);
	cc_update_nodeid();

#ifdef MODULE_CCCSHARE
	int32_t i;
	for(i = 0; i < CS_MAXPORTS; i++)
	{
		if(!cfg.cc_port[i])
		{
			break;
		}
		ph->ptab.ports[i].s_port = cfg.cc_port[i];
		ph->ptab.nports++;
	}

	if(cfg.cc_port[0])
	{
		cccam_init_share();
	}
#endif
}
#endif
