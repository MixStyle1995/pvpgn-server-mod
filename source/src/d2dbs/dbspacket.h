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
		typedef struct {
			t_d2dbs_d2gs_header	h;
		} t_d2gs_d2dbs_echoreply;

		// Thêm mới hoàn toàn:
		typedef struct {
			bn_int    size;    /* 4 bytes thay vì bn_short */
			bn_short  type;
			bn_int    seqno;
		} t_d2dbs_d2gs_header_ex;

#define D2GS_D2DBS_SAVE_DATA_REQUEST_EX  0x38
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_short  datatype;
			bn_int    datalen;     /* bn_int: không bị giới hạn 64KB */
		} t_d2gs_d2dbs_save_data_request_ex;

#define D2DBS_D2GS_SAVE_DATA_REPLY_EX    0x38
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_int    result;
			bn_short  datatype;
		} t_d2dbs_d2gs_save_data_reply_ex;

#define D2GS_D2DBS_GET_DATA_REQUEST_EX   0x39
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_short  datatype;
		} t_d2gs_d2dbs_get_data_request_ex;

#define D2DBS_D2GS_GET_DATA_REPLY_EX     0x39
		typedef struct {
			t_d2dbs_d2gs_header_ex  h;
			bn_int    result;
			bn_int    charcreatetime;
			bn_int    allowladder;
			bn_short  datatype;
			bn_int    datalen;     /* bn_int: không bị giới hạn 64KB */
		} t_d2dbs_d2gs_get_data_reply_ex;

#define D2GS_D2DBS_ECHOREPLY_EX		0x35
		typedef struct {
			t_d2dbs_d2gs_header_ex	h;
		} t_d2gs_d2dbs_echoreply_ex;

#define D2GS_D2DBS_SAVE_STASH_EX   0x3A
#define D2DBS_D2GS_SAVE_STASH_REPLY_EX 0x3A
		typedef struct 
		{
			t_d2dbs_d2gs_header_ex h;

			bn_int  stashtype;   // loại stash (shared, personal, hc, v.v.)
			bn_int  datasize;    // kích thước buffer

			/* AccountName */
			/* CharName */
			/* data */
		} t_d2gs_d2dbs_save_stash_request_ex;
		typedef struct {
			t_d2dbs_d2gs_header_ex h;

			bn_int result;   // 0 = OK, 1 = FAIL
		} t_d2dbs_d2gs_save_stash_reply_ex;

#define D2DBS_SAVE_STASH_SUCCESS 0
#define D2DBS_SAVE_STASH_FAILED  1

#pragma pack(pop)

		extern int dbs_packet_handle(t_d2dbs_connection * conn);
		extern int dbs_keepalive(void);
		extern int dbs_check_timeout(void);

	}

}

#endif
