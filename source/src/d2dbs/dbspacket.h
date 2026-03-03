/*
 * Copyright (C) 2001           faster  (lqx@cic.tsinghua.edu.cn)
 * Copyright (C) 2001		sousou	(liupeng.cs@263.net)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#ifndef INCLUDED_DBSPACKET_H
#define INCLUDED_DBSPACKET_H

#include "common/bn_type.h"
#include "dbserver.h"

namespace pvpgn
{

	namespace d2dbs
	{
#pragma pack(push, 1)
		typedef struct {
			bn_short  size;
			bn_short  type;
			bn_int    seqno;
		} t_d2dbs_d2gs_header;

		typedef struct {
			bn_byte   cclass;
		} t_d2gs_d2dbs_connect;
#define CONNECT_CLASS_D2GS_TO_D2DBS		0x65
#define CONNECT_CLASS_D2GS_TO_D2DBS_EX    0x66

#define D2GS_D2DBS_SAVE_DATA_REQUEST    0x30
		typedef struct {
			t_d2dbs_d2gs_header  h;
			bn_short      datatype;
			bn_short      datalen;
			/* AccountName */
			/* CharName */
			/* data */
		} t_d2gs_d2dbs_save_data_request;
#define D2GS_DATA_CHARSAVE    0x01
#define D2GS_DATA_PORTRAIT    0x02

#define D2DBS_D2GS_SAVE_DATA_REPLY      0x30
		typedef struct {
			t_d2dbs_d2gs_header  h;
			bn_int        result;
			bn_short      datatype;
			/* CharName */
		} t_d2dbs_d2gs_save_data_reply;
#define D2DBS_SAVE_DATA_SUCCESS    0
#define D2DBS_SAVE_DATA_FAILED    1

#define D2GS_D2DBS_GET_DATA_REQUEST    0x31
		typedef struct {
			t_d2dbs_d2gs_header  h;
			bn_short      datatype;
			/* AccountName */
			/* CharName */
		} t_d2gs_d2dbs_get_data_request;

#define D2DBS_D2GS_GET_DATA_REPLY    0x31
		typedef struct {
			t_d2dbs_d2gs_header  h;
			bn_int        result;
			bn_int    charcreatetime;
			bn_int    allowladder;
			bn_short      datatype;
			bn_short      datalen;
			/* CharName */
			/* data */
		} t_d2dbs_d2gs_get_data_reply;

#define D2DBS_GET_DATA_SUCCESS    0
#define D2DBS_GET_DATA_FAILED    1
#define D2DBS_GET_DATA_CHARLOCKED 2

#define D2GS_D2DBS_UPDATE_LADDER  0x32
		typedef struct {
			t_d2dbs_d2gs_header h;
			bn_int  charlevel;
			bn_int  charexplow;
			bn_int  charexphigh;
			bn_short charclass;
			bn_short charstatus;
			/* CharName */
			/* RealmName */
		} t_d2gs_d2dbs_update_ladder;

#define D2GS_D2DBS_CHAR_LOCK  0x33
		typedef struct {
			t_d2dbs_d2gs_header h;
			bn_int  lockstatus;
			/* CharName */
			/* RealmName */
		} t_d2gs_d2dbs_char_lock;

#define D2DBS_D2GS_ECHOREQUEST		0x34
		typedef struct {
			t_d2dbs_d2gs_header	h;
		} t_d2dbs_d2gs_echorequest;

#define D2GS_D2DBS_ECHOREPLY		0x34

/* =================================================================
 * Extended packets - large save file support (> 64KB)
 * Type 0x38 = EX_SAVE, 0x39 = EX_GET
 * Header mới: [bn_int size][bn_short type][bn_int seqno] = 10 bytes
 * D2GS cũ vẫn dùng 0x30/0x31 với header cũ, không bị ảnh hưởng
 * ================================================================= */

 /* Extended header: size là bn_int (4 bytes) thay vì bn_short */
		typedef struct {
			bn_int    size;    /* 4 bytes: hỗ trợ packet tới 4GB */
			bn_short  type;    /* 2 bytes */
			bn_int    seqno;   /* 4 bytes */
		} t_d2dbs_d2gs_header_ex;

		/* EX SAVE REQUEST: GS → DBS
		 * [t_d2dbs_d2gs_header_ex][bn_short datatype][bn_int datalen]
		 * [AccountName\0][CharName\0][RealmName\0][data...] */
#define D2GS_D2DBS_SAVE_DATA_REQUEST_EX  0x38
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_short  datatype;
			bn_int    datalen;
		} t_d2gs_d2dbs_save_data_request_ex;

		/* EX SAVE REPLY: DBS → GS
		 * [t_d2dbs_d2gs_header_ex][bn_int result][bn_short datatype][CharName\0] */
#define D2DBS_D2GS_SAVE_DATA_REPLY_EX    0x38
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_int    result;
			bn_short  datatype;
		} t_d2dbs_d2gs_save_data_reply_ex;

		/* EX GET REQUEST: GS → DBS
		 * [t_d2dbs_d2gs_header_ex][bn_short datatype]
		 * [AccountName\0][CharName\0][RealmName\0] */
#define D2GS_D2DBS_GET_DATA_REQUEST_EX   0x39
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_short  datatype;
		} t_d2gs_d2dbs_get_data_request_ex;

		/* EX GET REPLY: DBS → GS
		 * [t_d2dbs_d2gs_header_ex][bn_int result][bn_int charcreatetime]
		 * [bn_int allowladder][bn_short datatype][bn_int datalen][CharName\0][data...] */
#define D2DBS_D2GS_GET_DATA_REPLY_EX     0x39
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_int    result;
			bn_int    charcreatetime;
			bn_int    allowladder;
			bn_short  datatype;
			bn_int    datalen;
		} t_d2dbs_d2gs_get_data_reply_ex;


		/* =================================================================
		 * Stash packets — type 0x3A = STASH_SAVE, 0x3B = STASH_GET
		 * Dung header_ex (bn_int size) de ho tro file lon hon 64KB
		 *
		 * Layout STASH_SAVE request  (GS → DBS):
		 *   [header_ex][bn_int datalen][AccountName\0][CharName\0][data...]
		 *
		 * Layout STASH_SAVE reply    (DBS → GS):
		 *   [header_ex][bn_int result][CharName\0]
		 *
		 * Layout STASH_GET request   (GS → DBS):
		 *   [header_ex][AccountName\0][CharName\0]
		 *
		 * Layout STASH_GET reply     (DBS → GS):
		 *   [header_ex][bn_int result][bn_int datalen][CharName\0][data...]
		 *
		 * File luu tren dia: <charstash_dir>/<AccountName>/<CharName>.stash
		 * ================================================================= */

#define D2GS_D2DBS_STASH_SAVE_REQUEST   0x3A
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_int    datalen;        /* kich thuoc data */
			/* AccountName\0 */
			/* CharName\0    */
			/* data...        */
		} t_d2gs_d2dbs_stash_save_request;

#define D2DBS_D2GS_STASH_SAVE_REPLY     0x3A
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_int    result;
			/* CharName\0 */
		} t_d2dbs_d2gs_stash_save_reply;
#define D2DBS_STASH_SAVE_SUCCESS  0
#define D2DBS_STASH_SAVE_FAILED   1

#define D2GS_D2DBS_STASH_GET_REQUEST    0x3B
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			/* AccountName\0 */
			/* CharName\0    */
		} t_d2gs_d2dbs_stash_get_request;

#define D2DBS_D2GS_STASH_GET_REPLY      0x3B
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_int    result;
			bn_int    datalen;
			/* CharName\0 */
			/* data...     */
		} t_d2dbs_d2gs_stash_get_reply;
#define D2DBS_STASH_GET_SUCCESS   0
#define D2DBS_STASH_GET_FAILED    1

		typedef struct {
			t_d2dbs_d2gs_header	h;
		} t_d2gs_d2dbs_echoreply;

#pragma pack(pop)

		extern int dbs_packet_handle(t_d2dbs_connection * conn);
		extern int dbs_keepalive(void);
		extern int dbs_check_timeout(void);

	}

}

#endif
